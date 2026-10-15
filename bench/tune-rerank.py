"""tune-rerank.py DUMP [--exclude BENCH.jsonl --slice TUNE.jsonl] — pick the re-ranking weights offline.

DUMP comes from `swipekb-bench --dump` (every candidate's raw scores per swipe). This script re-ranks those
candidates with any weights exactly like swipekb::rerankCandidates (same formula, same "best raw shape stays
visible" guard) and searches for the weights with the most first-guess hits, ties broken by top-3.

WHY offline: one engine pass takes ~20 s per 2,000 swipes; re-ranking stored numbers takes milliseconds, so
thousands of weight settings can be compared. WHY a separate slice (FUTO's validation split, not the
scoreboard's test slice): weights tuned on the scoreboard would flatter the scoreboard.
--exclude drops swipes whose sentence also appears in the scoreboard slice, so not even a sentence is shared.

Needs numpy (scripts/lm uses a private venv: assets/lm-build/venv/bin/python -I bench/tune-rerank.py ...)."""
import argparse
import itertools
import json

import numpy as np


def load(dump, drop_ids):
    rows = [json.loads(l) for l in open(dump, encoding="utf-8")]
    rows = [r for r in rows if r["id"] not in drop_ids]
    n = max(len(r["c"]) for r in rows)
    S = len(rows)
    F = np.zeros((6, S, n), dtype=np.float64)   # ctc, len, freq, lm, futo_lm, valid
    tgt = np.full(S, -1)
    first = np.array([r["first"] for r in rows])
    for i, r in enumerate(rows):
        for j, c in enumerate(r["c"]):
            F[0:5, i, j] = c[1:6]
            F[5, i, j] = 1
            if c[0] == r["target"]:
                tgt[i] = j
    return F, tgt, first


def score(F, tgt, gamma, beta, lam, alpha, mu=0.0):
    ctc, L, f, lm, flm, valid = F
    s = ctc / np.maximum(L, 1) ** gamma + beta * L + lam * f + alpha * lm + mu * flm
    s = np.where(valid > 0, s, -np.inf)
    order = np.argsort(-s, axis=1, kind="stable")
    top1 = order[:, 0] == tgt
    in3 = (order[:, :3] == tgt[:, None]).any(axis=1)
    # The guard: the best raw-shape word takes slot 3 if it isn't shown already.
    best_ctc = np.argmax(np.where(valid > 0, ctc, -np.inf), axis=1)
    shown = (order[:, :3] == best_ctc[:, None]).any(axis=1)
    top3 = in3 & ~((~shown) & (order[:, 2] == tgt)) | ((~shown) & (best_ctc == tgt))
    return top1, top3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--exclude", help="scoreboard slice; drop tuning swipes sharing a sentence with it")
    ap.add_argument("--slice", help="the tuning slice the dump was made from (needed with --exclude)")
    ap.add_argument("--futo", action="store_true", help="also search a weight for FUTO's context model")
    ap.add_argument("--pin", choices=["nolm", "lm"],
                    help="keep FUTO's own path-fit weights (its no-context or with-context scoring.json row) and "
                         "sweep only alpha. WHY: searching all four weights over-fitted the tuning slice "
                         "(+1.2 points there, -0.1 on the scoreboard); one weight can't do that")
    ap.add_argument("--eval", help="just score these weights: gamma,beta,lambda,alpha")
    a = ap.parse_args()
    drop = set()
    if a.exclude and a.slice:
        bench = {json.loads(l)["sentence"] for l in open(a.exclude, encoding="utf-8")}
        drop = {json.loads(l)["id"] for l in open(a.slice, encoding="utf-8") if json.loads(l)["sentence"] in bench}
    F, tgt, first = load(a.dump, drop)
    N = len(tgt)
    print(f"{N} swipes ({len(drop)} dropped for sharing a sentence with the scoreboard); "
          f"target in pool: {(tgt >= 0).mean() * 100:.2f}%")

    def report(name, w):
        t1, t3 = score(F, tgt, *w)
        print(f"{name:28s} top1 {t1.mean() * 100:6.2f}  top3 {t3.mean() * 100:6.2f}  "
              f"first-word top1 {t1[first].mean() * 100:6.2f}   {tuple(round(x, 4) for x in w)}")
        return t1.mean(), t3.mean()

    report("FUTO no-LM row, alpha 0", (0.5949, 0.7271, 0.0134, 0.0))
    report("FUTO LM row (futo_lm)", (0.1126, 2.2138, 0.0060, 0.0, 0.6387))
    if a.eval:
        report("eval", tuple(float(x) for x in a.eval.split(",")))
        return
    if a.pin:
        g, b, l = (0.5949, 0.7271, 0.0134) if a.pin == "nolm" else (0.1126, 2.2138, 0.0060)
        res = []
        for al in np.arange(0.0, 3.01, 0.125):
            t1, t3 = score(F, tgt, g, b, l, al)
            res.append((t1.sum(), t3.sum(), al))
            report(f"  alpha {al:.3f}", (g, b, l, al))
        # The middle of the best plateau, not its edge: average the alphas within one swipe of the best.
        top = max(r[0] for r in res)
        near = [r[2] for r in res if r[0] >= top - 1]
        print(f"best top1 {top / N * 100:.2f} for alpha in [{min(near)}, {max(near)}] -> middle "
              f"{(min(near) + max(near)) / 2:.3f}")
        return

    best = None
    # Coarse grid, then refine around the best point twice.
    grids = {
        "gamma": [0.2, 0.4, 0.6, 0.8, 1.0],
        "beta": [0.0, 0.4, 0.8, 1.2, 1.6, 2.4],
        "lam": [0.0, 0.005, 0.01, 0.02],
        "alpha": [0.5, 1.0, 1.5, 2.0, 3.0, 4.0],
        "mu": [0.0, 0.3, 0.6] if a.futo else [0.0],
    }
    for rnd in range(3):
        for w in itertools.product(*grids.values()):
            t1, t3 = score(F, tgt, *w)
            key = (t1.sum(), t3.sum())
            if best is None or key > best[0]:
                best = (key, w)
        g, b, l, al, mu = best[1]
        step = 0.5 ** (rnd + 1)
        grids = {
            "gamma": sorted({max(0, g + d * 0.2 * step) for d in (-2, -1, 0, 1, 2)}),
            "beta": sorted({max(0, b + d * 0.4 * step) for d in (-2, -1, 0, 1, 2)}),
            "lam": sorted({max(0, l + d * 0.005 * step) for d in (-2, -1, 0, 1, 2)}),
            "alpha": sorted({max(0, al + d * 0.5 * step) for d in (-2, -1, 0, 1, 2)}),
            "mu": sorted({max(0, mu + d * 0.3 * step) for d in (-1, 0, 1)}) if a.futo else [0.0],
        }
    report("best", best[1])
    # Plateau check: how much each weight can move before top-1 drops (over-fitting shows as a narrow spike).
    g, b, l, al, mu = best[1]
    for name, idx, deltas in (("alpha", 3, (-0.5, -0.25, 0.25, 0.5)), ("gamma", 0, (-0.1, 0.1)),
                              ("beta", 1, (-0.2, 0.2)), ("lambda", 2, (-0.005, 0.005))):
        for d in deltas:
            w = list(best[1])
            w[idx] = max(0, w[idx] + d)
            report(f"  {name} {d:+}", tuple(w))


if __name__ == "__main__":
    main()
