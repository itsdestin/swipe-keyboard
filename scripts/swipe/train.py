"""CTC training of the swipe net. CPU for smoke tests, CUDA for the real run (python -I train.py ...).

  train.py --train dev.npz [--train synth.npz ...] --val devhold.npz --out run1 --epochs 40 --device cuda
Outputs in --out: model.pt (best by validation loss), model.bin (runner format), log.txt.
"""
import argparse
import os
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402


class Data:
    """One or more npz files, each with its own layout (key indices in the labels refer to that layout).

    WHY padded key lists: a batch can mix English QWERTY, French AZERTY, Spanish (n-tilde) ... so keys are padded to
    the largest layout and the padding is masked out of the softmax."""

    def __init__(self, specs, limit=0):
        self.layouts, parts, src = [], [], []
        for i, (path, layout_path) in enumerate(specs):
            self.layouts.append(S.Layout(layout_path))
            z = np.load(path)
            parts.append(z)
            src.append(np.full(len(z["lens"]), i, np.int32))
        self.feats = np.concatenate([p["feats"] for p in parts])
        self.pos = np.concatenate([p["pos"] for p in parts])
        self.aspect = np.concatenate([p["aspect"] for p in parts])
        self.lens = np.concatenate([p["lens"] for p in parts]).astype(np.int64)
        self.words = np.concatenate([p["words"] for p in parts])
        self.src = np.concatenate(src)
        self.labels = np.concatenate([p["labels"] for p in parts]).astype(np.int64)
        self.offs = np.concatenate([[0], np.cumsum(self.lens)])
        if limit:
            self.feats, self.pos, self.aspect, self.lens, self.words, self.src = (
                a[:limit] for a in (self.feats, self.pos, self.aspect, self.lens, self.words, self.src))
        self.n = len(self.lens)
        self.kmax = max(len(l.labels) for l in self.layouts)
        self.cache = {}

    def keys(self, idx):
        ks, ms = [], []
        for i in idx:
            key = (int(self.src[i]), float(self.aspect[i]))
            v = self.cache.get(key)
            if v is None:
                lay = self.layouts[key[0]]
                k = lay.key_array(key[1]).astype(np.float32)
                pad = np.tile(np.array([[-1000.0, -1000.0, 1.0, 1.0]], np.float32), (self.kmax - len(k), 1))
                m = np.concatenate([np.ones(len(k), bool), np.zeros(len(pad), bool)])
                v = (np.concatenate([k, pad]), m)
                self.cache[key] = v
            ks.append(v[0]); ms.append(v[1])
        return np.stack(ks), np.stack(ms)

    def batch(self, idx, device, aug=False, rng=None, time_aug=0.0):
        feats = self.feats[idx].copy()
        pos = self.pos[idx].copy()
        asp = self.aspect[idx]
        if aug:
            # WHY: real users' touches are offset and uneven; shifting the whole trace teaches the net to rely on
            # shape/relative position rather than exact pixels. ~0.2 key widths is in line with real start errors.
            units = np.array([self.layouts[s].unit for s in self.src[idx]], np.float32)[:, None]
            off = rng.normal(0, 0.2, size=(len(idx), 1, 2)).astype(np.float32)
            pos += off
            feats[:, :, 0] += off[:, :, 0] / units
            feats[:, :, 1] += off[:, :, 1] / (units * asp[:, None])
        if aug and time_aug > 0:
            # WHY: people swipe at very different speeds and pause in different places; rescaling the elapsed-time
            # cue per swipe (and a little per step) keeps the net from memorising one speed profile.
            dt = np.expm1(4.0 * feats[:, :, 4])
            scale = np.exp(rng.normal(0, time_aug, size=(len(idx), 1))) * np.exp(rng.normal(0, time_aug / 3, size=dt.shape))
            feats[:, :, 4] = np.log1p(dt * scale) / 4.0
        tg = np.concatenate([self.labels[self.offs[i]:self.offs[i + 1]] + 1 for i in idx])
        keys, mask = self.keys(idx)
        return (torch.from_numpy(feats).to(device), torch.from_numpy(pos).to(device),
                torch.from_numpy(keys).to(device), torch.from_numpy(mask).to(device),
                torch.from_numpy(tg).to(device), torch.from_numpy(self.lens[idx]).to(device))


def greedy_exact(lp, lens, labels_by_item):
    """Fraction of swipes whose greedy CTC key sequence equals the target key sequence."""
    best = lp.argmax(-1).cpu().numpy()
    ok = 0
    for b in range(best.shape[0]):
        seq, prev = [], 0
        for c in best[b]:
            if c != prev and c != 0:
                seq.append(int(c) - 1)
            prev = c
        ok += seq == labels_by_item[b]
    return ok / best.shape[0]


def evaluate(net, data, device, batch=512):
    net.eval()
    tot, n, ok = 0.0, 0, 0.0
    with torch.no_grad():
        for s in range(0, data.n, batch):
            idx = np.arange(s, min(s + batch, data.n))
            feats, pos, keys, mask, tg, tl = data.batch(idx, device)
            lp = net(feats, pos, keys, mask)
            loss = F.ctc_loss(lp.transpose(0, 1), tg, torch.full((len(idx),), lp.shape[1], dtype=torch.long), tl,
                              blank=0, reduction="sum", zero_infinity=True)
            tot += loss.item(); n += len(idx)
            lab = [list(data.labels[data.offs[i]:data.offs[i + 1]]) for i in idx]
            ok += greedy_exact(lp, tl, lab) * len(idx)
    return tot / n, ok / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train", action="append", required=True, help="file.npz or file.npz:layout.tsv (repeatable)")
    ap.add_argument("--val", action="append", required=True, help="same form; several allowed, model chosen by mean loss")
    ap.add_argument("--layout", default=os.path.join(os.path.dirname(__file__), "layouts", "qwerty.tsv"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--device", default="cpu")
    ap.add_argument("--limit", type=int, default=0, help="use only the first N training swipes (smoke tests)")
    ap.add_argument("--val-limit", type=int, default=0)
    ap.add_argument("--conv", default="64,64")
    ap.add_argument("--hidden", type=int, default=96)
    ap.add_argument("--layers", type=int, default=2)
    ap.add_argument("--pointer", type=int, default=32)
    ap.add_argument("--no-aug", action="store_true")
    ap.add_argument("--time-aug", type=float, default=0.0, help="std of log speed rescaling (0 = off)")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    if a.threads:
        torch.set_num_threads(a.threads)
    torch.manual_seed(a.seed)
    rng = np.random.default_rng(a.seed)
    os.makedirs(a.out, exist_ok=True)
    def specs(items):
        out = []
        for it in items:
            path, _, lay = it.partition(":")
            out.append((path, lay or a.layout))
        return out
    tr = Data(specs(a.train), a.limit)
    vals = [Data(specs([v]), a.val_limit) for v in a.val]
    cfg = dict(conv=[int(v) for v in a.conv.split(",")], hidden=a.hidden, layers=a.layers, pointer=a.pointer)
    net = S.build_net(cfg).to(a.device)
    nparam = sum(p.numel() for p in net.parameters())
    steps_per_epoch = (tr.n + a.batch - 1) // a.batch
    opt = torch.optim.AdamW(net.parameters(), lr=a.lr, weight_decay=1e-2)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.epochs * steps_per_epoch, pct_start=0.15)
    log = open(os.path.join(a.out, "log.txt"), "a")

    def say(msg):
        print(msg, flush=True); log.write(msg + "\n"); log.flush()

    say(f"params {nparam}  train {tr.n}  val {[v.n for v in vals]}  cfg {cfg}  device {a.device}")
    best = 1e9
    for ep in range(a.epochs):
        net.train()
        t0 = time.time()
        perm = rng.permutation(tr.n)
        run = 0.0
        for s in range(0, tr.n, a.batch):
            idx = perm[s:s + a.batch]
            feats, pos, keys, mask, tg, tl = tr.batch(idx, a.device, aug=not a.no_aug, rng=rng, time_aug=a.time_aug)
            lp = net(feats, pos, keys, mask)
            loss = F.ctc_loss(lp.transpose(0, 1), tg, torch.full((len(idx),), lp.shape[1], dtype=torch.long), tl,
                              blank=0, reduction="mean", zero_infinity=True)
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(net.parameters(), 5.0)
            opt.step(); sched.step()
            run += loss.item() * len(idx)
        res = [evaluate(net, v, a.device) for v in vals]
        vl = sum(r[0] for r in res) / len(res)
        per = " ".join(f"[{r[0]:.3f}/{r[1] * 100:.1f}%]" for r in res)
        say(f"epoch {ep + 1}/{a.epochs} train_loss {run / tr.n:.3f} val_loss {vl:.3f} val_greedy_keyseq_acc {res[0][1] * 100:.1f}% sets {per} ({time.time() - t0:.0f}s)")
        if vl < best:
            best = vl
            torch.save({"cfg": cfg, "state": net.state_dict()}, os.path.join(a.out, "model.pt"))
            size = S.export_model(net, os.path.join(a.out, "model.bin"))
    say(f"best val_loss {best:.3f}; model.bin {size} bytes")


if __name__ == "__main__":
    main()
