#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""scripts/swipe/eval_swap.py — joint-model step 4: judge a trial swipe model on the WHOLE pipeline (search + learned
re-ranker) before the C++ engine can run it, by swapping its path fit into the engine's own found words.

Input: a features dump written by `swipekb-rescore-bench --features FILE --with-words` with the shipped model (each
swipe's best 30 words, their re-ranker features, search score and path fit), the same swipe file, the shipped model's
model.pt and the trial model's model.pt, and the re-ranker weights (the text format of engine/src/swipe_rerank.cpp).

For every word: delta = trial path fit - shipped path fit (both a full CTC forward over the word's keys, computed here
on the same network input), added to the engine's path fit and search score. The list is re-sorted by the new search
score, the features that depend on the path fit or on the order (search score, path fit, first word, place, lead over
the second, place by path fit, place by neural score) are recomputed exactly as LanguageEngine::rerankFeatures does,
and the re-ranker orders the 30 words. WHY a delta and not the trial's score alone: the engine's path fit comes from
its pruned search; the delta keeps the engine's numbers and changes only what the trial model changes.
Limits (stated with the results): a word the shipped search did not find cannot appear, and the re-ranker was trained
on the shipped model's numbers.

Checks: `--trial` equal to `--base` must reproduce the engine's re-ranked first guesses (see the report); test sets are
refused (this is for validation swipes only).

Usage: eval_swap.py --features FEAT.jsonl --swipes SWIPES.jsonl --layout L.tsv --base T1.pt --trial X.pt
                    --reranker rerank.txt [--device cuda] [--write-features DIR]
  --write-features DIR: also writes each trial's swapped lists in the --features format (DIR/<trial>/<same file name>),
  so train_rerank.py train can fit a re-ranker to the trial model's numbers.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402

FEATURES = ["score", "path", "prior", "geo", "neural", "first", "rank_log", "freq", "letters", "length_fit", "start",
            "end", "apostrophe", "first_gap", "path_rank", "neural_rank", "swipe_length"]
FI = {f: i for i, f in enumerate(FEATURES)}
REFUSE = ("bench-2000", "test-extra", "test.jsonl", "heliboard", "futo-scorable")


def read_reranker(path):
    """{set name: dict(hidden, mean, scale, lin, w1, b1, out)} from the SwipeReranker text format."""
    sets, cur = {}, None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "set":
            cur = sets[p[1]] = {"hidden": int(p[3])}
        elif cur is not None and p[0] in ("mean", "scale", "lin", "w1", "b1", "out"):
            cur[p[0]] = np.array([float(v) for v in p[1:]], np.float64)
    for m in sets.values():
        if m["hidden"]:
            m["w1"] = m["w1"].reshape(m["hidden"], len(FEATURES))
    return sets


def rerank_score(m, X):
    """SwipeReranker::score for rows X (n, 17): non-finite features count as the training mean."""
    X = np.where(np.isfinite(X), X, m["mean"])
    z = (X - m["mean"]) / m["scale"]
    s = z @ m["lin"]
    if m["hidden"]:
        s = s + np.maximum(z @ m["w1"].T + m["b1"], 0.0) @ m["out"]
    return s


def recompute(feats, score, ctc, neural_ok):
    """The order-dependent features of one swipe's list (already in the new search order), as rerankFeatures."""
    n = len(score)
    F = feats.copy()
    clip = lambda v, lo, hi: min(max(v, lo), hi)
    best_path = max(ctc)
    nat = F[:, FI["neural"]]   # relative neural score: ranks are the same as on the absolute one
    for i in range(n):
        F[i, FI["score"]] = clip(score[i] - score[0], -40, 0)
        F[i, FI["path"]] = clip(ctc[i] - best_path, -40, 0)
        F[i, FI["first"]] = 1.0 if i == 0 else 0.0
        F[i, FI["rank_log"]] = math.log1p(i)
        F[i, FI["first_gap"]] = (clip(score[0] - score[1], 0, 10) if n > 1 else 10.0) if i == 0 else 0.0
        pr = sum(1 for j in range(n) if j != i and (ctc[j] > ctc[i] or (ctc[j] == ctc[i] and j < i)))
        F[i, FI["path_rank"]] = math.log1p(pr)
        if neural_ok:
            nr = sum(1 for j in range(n) if j != i and (nat[j] > nat[i] or (nat[j] == nat[i] and j < i)))
            F[i, FI["neural_rank"]] = math.log1p(nr)
    return F


def load_net(path, device):
    import torch
    ck = torch.load(path, map_location=device)
    net = S.build_net(ck["cfg"]).to(device)
    net.load_state_dict(ck["state"])
    net.eval()
    return net


def path_fits(nets, recs, words, layout, device, has_ctx):
    """For each net: list (per swipe) of arrays (per word) of CTC log-likelihoods (nats). has_ctx[i]: the engine had
    words before swipe i (a joint net then reads them; otherwise it gets none, as the keyboard would give it)."""
    import torch
    import torch.nn.functional as Fn
    keys_cache = {}
    out = [[] for _ in nets]
    B = 256
    for s0 in range(0, len(recs), B):
        chunk = range(s0, min(s0 + B, len(recs)))
        feats, pos, kk, ctx = [], [], [], []
        for i in chunk:
            r = recs[i]
            asp = r["canvas_height"] / r["canvas_width"]
            d = r["data"]
            f, p = S.trace_to_input([q["x"] for q in d], [q["y"] for q in d], [q["t"] for q in d], asp, layout)
            feats.append(f); pos.append(p)
            k = keys_cache.get(asp)
            if k is None:
                k = keys_cache[asp] = layout.key_array(asp).astype(np.float32)
            kk.append(k)
            ctx.append(S.context_ids(S.words_before(r)) if has_ctx[i] else [0] * S.CTX_CHARS)
        T = lambda a, dt=torch.float32: torch.tensor(np.stack(a), dtype=dt, device=device)
        feats_t, pos_t, keys_t, ctx_t = T(feats), T(pos), T(kk), T(ctx, torch.long)
        # one CTC row per (swipe, word)
        rows, tg, tl = [], [], []
        for bi, i in enumerate(chunk):
            for w in words[i]:
                ks = layout.word_keys(w)
                rows.append(bi)
                tg.extend(k + 1 for k in ks)
                tl.append(len(ks))
        with torch.no_grad():
            for ni, net in enumerate(nets):
                lp = net(feats_t, pos_t, keys_t, None, ctx_t if net.cfg.get("ctx") else None)
                idx = torch.tensor(rows, dtype=torch.long, device=device)
                lpr = lp[idx]                                     # (R, T, 1+K)
                tl_t = torch.tensor(tl, dtype=torch.long, device=device)
                good = tl_t > 0
                tg_t = torch.tensor(tg, dtype=torch.long, device=device)
                nll = Fn.ctc_loss(lpr.transpose(0, 1), tg_t, torch.full((len(rows),), lpr.shape[1], dtype=torch.long),
                                  tl_t, blank=0, reduction="none", zero_infinity=False)
                ll = (-nll).double().cpu().numpy()
                ll[~good.cpu().numpy()] = 0.0
                ll[~np.isfinite(ll)] = -1e4
                k = 0
                for bi, i in enumerate(chunk):
                    n = len(words[i])
                    out[ni].append(ll[k:k + n])
                    k += n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--features", required=True)
    ap.add_argument("--swipes", required=True)
    ap.add_argument("--layout", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--trial", action="append", required=True, help="trial model.pt (repeatable)")
    ap.add_argument("--reranker", required=True)
    ap.add_argument("--device", default="cpu")
    ap.add_argument("--write-features", default=None, help="folder for the swapped feature lists (re-training the re-ranker)")
    a = ap.parse_args()
    for p in (a.features, a.swipes):
        if any(t in os.path.basename(p).lower() for t in REFUSE):
            raise SystemExit(f"refusing {p}: test sets are scored once, with the real tools")
    use_ctx = not a.features.endswith("-noctx.jsonl")
    lines = [json.loads(l) for l in open(a.features)]
    recs = [json.loads(l) for l in open(a.swipes)]
    if len(lines) != len(recs):
        raise SystemExit(f"{len(lines)} dump lines but {len(recs)} swipes")
    layout = S.Layout(a.layout)
    rr = read_reranker(a.reranker)
    words = [[w[0] for w in L.get("w", [])] for L in lines]
    nets = [load_net(a.base, a.device)] + [load_net(t, a.device) for t in a.trial]
    fits = path_fits(nets, recs, words, layout, a.device, [use_ctx and bool(L["before"]) for L in lines])
    names = ["base"] + [os.path.basename(os.path.dirname(t)) or t for t in a.trial]
    res = {}
    saved = {}
    for ni, name in enumerate(names):
        first = top3 = search_first = n = 0
        rows_out = []
        for si, L in enumerate(lines):
            n += 1
            c = L["c"]
            if not c:
                rows_out.append(None)
                continue
            labels = np.array([x[0] for x in c])
            feats = np.array([x[1:] for x in c], np.float64)
            score = np.array([w[1] for w in L["w"]], np.float64)
            ctc = np.array([w[2] for w in L["w"]], np.float64)
            d = fits[ni][si] - fits[0][si]
            score, ctc = score + d, ctc + d
            order = sorted(range(len(c)), key=lambda i: (-score[i], i))   # stable re-sort by the new search score
            labels, feats, score, ctc = labels[order], feats[order], score[order], ctc[order]
            before = bool(L["before"]) and use_ctx
            F = recompute(feats, list(score), list(ctc), before)
            rows_out.append((before, labels, F))
            search_first += int(labels[0])
            m = rr["sentence" if before else "nosentence"]
            s = rerank_score(m, F)
            rk = sorted(range(len(s)), key=lambda i: (-s[i], i))
            first += int(labels[rk[0]])
            top3 += int(any(labels[i] for i in rk[:3]))
        res[name] = (n, first, top3, search_first)
        saved[name] = rows_out
        print(f"{name:24s} swipes {n}  first {first} ({100 * first / n:.2f}%)  top3 {100 * top3 / n:.2f}%  "
              f"search-only first {100 * search_first / n:.2f}%", flush=True)
    if a.write_features:
        for name, rows in saved.items():
            if name == "base":
                continue
            d = os.path.join(a.write_features, name)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, os.path.basename(a.features)), "w") as f:
                for L, r in zip(lines, rows):
                    c = [] if r is None else [[int(lab)] + [float(f"{v:.5g}") for v in row] for lab, row in zip(r[1], r[2])]
                    f.write(json.dumps({"before": L["before"], "right": int(bool(c) and c[0][0]), "c": c}) + "\n")


if __name__ == "__main__":
    main()
