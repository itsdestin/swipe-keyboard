#!/usr/bin/env python3
"""Pick the blend weights (lambda, beta) of a language's neural model on sentences the scoreboard never uses.

  tune.py EVAL MODEL.ngram VOCAB.tsv NWP.bin TESTSETS_DIR LANG [--skip 1000] [--limit 300]

Runs swipekb-nextword-eval over a grid for the three typing measures (next word, completion after 1 and after
3 letters) on every test set, and prints the best pair plus the whole table.
WHY lines 1000+ of each test file: bench/scoreboard.sh reports the first 300, so tuning on other sentences
keeps the reported figures honest (the sets themselves are already excluded from training).
WHY the mean of the measures as the objective: one weight pair serves all three situations; the mean over the
three sets keeps casual, web and Tatoeba text balanced.
"""
import argparse
import itertools
import subprocess
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument("eval"); ap.add_argument("model"); ap.add_argument("vocab"); ap.add_argument("nwp")
ap.add_argument("testsets"); ap.add_argument("lang")
ap.add_argument("--skip", type=int, default=1000)
ap.add_argument("--limit", type=int, default=300)
ap.add_argument("--lambdas", default="0.4,0.55,0.7,0.85,1.0")
ap.add_argument("--betas", default="-1,-0.5,0,0.5,1")
a = ap.parse_args()


def run(job):
    lam, beta, st, k = job
    out = subprocess.run([a.eval, "--model", a.model, "--vocab", a.vocab, "--nwp", a.nwp, "--sentences",
                          f"{a.testsets}/{a.lang}/test/{st}.txt", "--skip", str(a.skip), "--limit", str(a.limit),
                          "--letters", str(k), "--lambda", str(lam), "--beta", str(beta)],
                         capture_output=True, text=True, check=True).stdout.splitlines()[1].split("\t")
    # columns: 3 = top3%, 10 = top2%  (see the program's header)
    return float(out[3] if k == 0 else out[10])


grid = list(itertools.product([float(x) for x in a.lambdas.split(",")], [float(x) for x in a.betas.split(",")]))
jobs = [(l, b, st, k) for l, b in grid for st in ("everyday", "web", "tatoeba") for k in (0, 1, 3)]
with ThreadPoolExecutor(24) as pool:
    res = list(pool.map(run, jobs))
rows = []
for i, (l, b) in enumerate(grid):
    r = res[i * 9:(i + 1) * 9]
    rows.append((sum(r) / 9, l, b, r))
rows.sort(reverse=True)
for mean, l, b, r in rows[:10]:
    print(f"lambda={l} beta={b} mean={mean:.2f}  " + " ".join(f"{x:.1f}" for x in r))
print(f"BEST lambda={rows[0][1]} beta={rows[0][2]}")
