#!/usr/bin/env python3
"""Train one language's next-word model (NWP LM, card #170): a one-layer LSTM with a projection, tied
input/output word vectors - the size class of Google's published Gboard next-word models (arXiv 2305.18465).

  train.py DATADIR OUTDIR [--dim 128] [--hidden 1024] [--tokens-per-step 24576] [--epochs 1.0]
                          [--lr 2e-3] [--minutes N]

DATADIR holds lm_vocab.txt and train.u16 from prepare.py. Writes OUTDIR/ckpt.pt (atomic, every few minutes
and at the end) and prints validation perplexity on 20,000 held-out sentences.
WHY whole sentences (not a running token stream): the keyboard only ever feeds the model the current sentence
starting from <s>, so training starts every sentence from <s> with zero state - nothing is learned from
text the keyboard will never provide. WHY length buckets: cuDNN needs rectangular batches; sentences of
similar length waste almost no padding.
WHY projection (proj_size = dim): hidden 1024 gives the LSTM capacity while the output layer, which dominates
cost (one dot product per vocabulary word), only sees dim numbers - and lets the output reuse the input word
vectors (tying), so the vocabulary table is stored once.
"""
import argparse
import math
import os
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

BOS, EOS = 1, 2
MAXLEN = 40
BUCKETS = (8, 16, 24, 32, 40)


class NWP(nn.Module):
    def __init__(self, vocab, dim, hidden):
        super().__init__()
        self.emb = nn.Embedding(vocab, dim)
        self.lstm = nn.LSTM(dim, hidden, proj_size=dim, batch_first=True)
        self.bias = nn.Parameter(torch.zeros(vocab))
        nn.init.normal_(self.emb.weight, std=0.1)

    def forward(self, x, state=None):
        h, state = self.lstm(self.emb(x), state)
        return F.linear(h, self.emb.weight, self.bias), state


def load(datadir, device):
    vocab = len(open(os.path.join(datadir, "lm_vocab.txt"), encoding="utf-8").read().split("\n")) - 1 + 3
    tok = torch.from_numpy(np.fromfile(os.path.join(datadir, "train.u16"), dtype="<u2").view(np.int16)).to(device)
    ends = (tok == EOS).nonzero().flatten()
    starts = torch.cat([torch.zeros(1, dtype=ends.dtype, device=device), ends[:-1] + 1])
    lens = ends - starts + 1                       # includes the </s> target
    return vocab, tok, starts, lens


def batch(tok, starts, lens, sel, L):
    """sel: sentence indices. Returns input [B,L], target [B,L] (-100 = padding)."""
    ar = torch.arange(L, device=tok.device)
    pos = starts[sel, None] + ar[None, :]
    valid = ar[None, :] < lens[sel, None]
    tgt = tok[pos.clamp(max=tok.numel() - 1)].long()
    tgt = torch.where(valid, tgt, torch.full_like(tgt, -100))
    inp = torch.cat([torch.full((len(sel), 1), BOS, device=tok.device), tgt[:, :-1].clamp(min=0)], 1)
    return inp, tgt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("datadir")
    ap.add_argument("outdir")
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--hidden", type=int, default=1024)
    ap.add_argument("--tokens-per-step", type=int, default=24576)
    ap.add_argument("--epochs", type=float, default=1.0)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--minutes", type=float, default=0, help="stop and finish the schedule early after this long")
    ap.add_argument("--max-steps", type=int, default=0)
    a = ap.parse_args()
    dev = "cuda"
    torch.manual_seed(1)
    os.makedirs(a.outdir, exist_ok=True)
    vocab, tok, starts, lens = load(a.datadir, dev)
    n = len(starts)
    perm = torch.randperm(n, device=dev)
    held, train_sel = perm[:20000], perm[20000:]
    cap = lens.clamp(max=MAXLEN)
    bucket = torch.bucketize(cap, torch.tensor(BUCKETS, device=dev))     # 0..4
    pools = [train_sel[bucket[train_sel] == b] for b in range(len(BUCKETS))]
    mass = torch.tensor([float(cap[p].sum()) for p in pools])
    total_tokens = float(mass.sum())
    steps = int(a.epochs * total_tokens / a.tokens_per_step)
    if a.max_steps:
        steps = min(steps, a.max_steps)
    print(f"vocab={vocab} sentences={n} tokens={int(total_tokens)} steps={steps}", flush=True)
    model = NWP(vocab, a.dim, a.hidden).to(dev)
    print("params", sum(p.numel() for p in model.parameters()), flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=0.0, betas=(0.9, 0.98))
    warm = 300

    def lr_at(s):
        if s < warm:
            return a.lr * (s + 1) / warm
        return a.lr * (0.03 + 0.97 * 0.5 * (1 + math.cos(math.pi * min(1.0, (s - warm) / max(1, steps - warm)))))

    def evaluate():
        model.eval()
        tot = cnt = 0
        with torch.no_grad(), torch.autocast("cuda", dtype=torch.bfloat16):
            for b in range(len(BUCKETS)):
                sel_all = held[bucket[held] == b]
                for i in range(0, len(sel_all), 256):
                    inp, tgt = batch(tok, starts, lens, sel_all[i:i + 256], BUCKETS[b])
                    logits, _ = model(inp)
                    tot += float(F.cross_entropy(logits.float().flatten(0, 1), tgt.flatten(), ignore_index=-100, reduction="sum"))
                    cnt += int((tgt != -100).sum())
        model.train()
        return math.exp(tot / cnt)

    def save():
        torch.save({"model": model.state_dict(), "vocab": vocab, "dim": a.dim, "hidden": a.hidden}, os.path.join(a.outdir, "ckpt.pt.tmp"))
        os.replace(os.path.join(a.outdir, "ckpt.pt.tmp"), os.path.join(a.outdir, "ckpt.pt"))

    probs = (mass / mass.sum())
    t0 = time.time(); last_save = t0; seen = 0
    for s in range(steps):
        if a.minutes and time.time() - t0 > a.minutes * 60:
            break
        for g in opt.param_groups:
            g["lr"] = lr_at(s)
        b = int(torch.multinomial(probs, 1))
        L = BUCKETS[b]
        B = max(1, a.tokens_per_step // L)
        sel = pools[b][torch.randint(len(pools[b]), (B,), device=dev)]
        inp, tgt = batch(tok, starts, lens, sel, L)
        with torch.autocast("cuda", dtype=torch.bfloat16):
            logits, _ = model(inp)
        loss = F.cross_entropy(logits.float().flatten(0, 1), tgt.flatten(), ignore_index=-100)
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        seen += B * L
        if s % 200 == 0:
            print(f"step {s}/{steps} loss {loss.item():.3f} tok/s {seen / (time.time() - t0):.0f} lr {lr_at(s):.5f}", flush=True)
        if s % 2000 == 1999:
            print(f"  val ppl {evaluate():.2f}", flush=True)
        if time.time() - last_save > 600:
            save(); last_save = time.time()
    save()
    print(f"final val ppl {evaluate():.2f}", flush=True)


if __name__ == "__main__":
    main()
