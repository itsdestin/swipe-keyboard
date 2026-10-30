#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""bench/rescore-tune.py — tunes ContextRescore (the neural next-word model re-scoring a swipe's best words; joint-model
experiment step 1) on TUNING data only.

For each set, `swipekb-rescore-bench --dump` writes, per swipe, the 30 words the swipe search found (the very list
LanguageEngine::recognize re-scores) with their score, n-gram prior, neural score and whether each is the right word.
The re-scored first guess is then worked out here for every pair of weights, so the whole grid costs one replay:
  new score = score + (ngram - 1) * prior + neural_weight * ln(10) * neural     (swipes with words before only)
Grid: neural in {0, .25, .5, .75, 1, 1.5, 2, 3}, ngram in {0, .25, .5, .75, 1} (WHY coarse: French and German have
only 648 tuning swipes, about 60 wrong first guesses; a fine grid would fit their noise).
Objective: right first guesses (the "no tap" share of the UX scoreboard). Ties: the pair closest to no change (0, 1).
Groups: English = trv8k; French + German pooled (tune-fr, tune-de; Spanish has no tuning set and takes their weights).
The top 3 printed here is the re-scored list's first three words before the keyboard's rules for the other places
(junk, regional spelling), so it is a guide only; the real top 3 comes from swipekb-bench --rescore.

Usage: bench/rescore-tune.py [--dumps DIR] [--bench PATH] [-j N] [--out FILE]
  --dumps  folder for (and of already made) dumps; a set's dump is made only when missing.
Env: SWIPEKB_ASSETS (language files in its language-files/), SWIPEKB_TUNE_DIR (~/.cache/swipe-keyboard-train/shape/futo).
"""
import argparse
import concurrent.futures as cf
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNE = os.environ.get("SWIPEKB_TUNE_DIR", os.path.expanduser("~/.cache/swipe-keyboard-train/shape/futo"))
LANG_FILES = os.path.join(os.environ.get("SWIPEKB_ASSETS", os.path.join(ROOT, "assets")), "language-files")
# WHY only these: the test sets (bench-2000, test-extra-10000, HeliBoard) are never tuned on.
SETS = {
    "trv8k": (f"{TUNE}/trv8k.jsonl", "en", f"{ROOT}/scripts/swipe/layouts/qwerty.tsv", "ascii"),
    "tune-fr": (f"{TUNE}/s5-azerty-val.jsonl", "fr", f"{TUNE}/layouts/azerty.tsv", "lookup"),
    "tune-de": (f"{TUNE}/s5-german-val.jsonl", "de", f"{TUNE}/layouts/german.tsv", "lookup"),
}
GROUPS = {"en": ["trv8k"], "fr+de (es)": ["tune-fr", "tune-de"]}
NEURAL = [0, 0.25, 0.5, 0.75, 1, 1.5, 2, 3]
NGRAM = [0, 0.25, 0.5, 0.75, 1]
LN10 = math.log(10)


def make_dump(bench, name, path):
    swipes, lang, layout, match = SETS[name]
    d = f"{LANG_FILES}/{'en_swipe' if lang == 'en' else lang}.combined"
    cmd = ["nice", "-n", "10", bench, "--dict", d, "--model", f"{LANG_FILES}/swipe-model.bin",
           "--ngram", f"{LANG_FILES}/{lang}.ngram", "--vocab", f"{LANG_FILES}/{lang}.vocab.tsv",
           "--nwp", f"{LANG_FILES}/{lang}.nwp.bin", "--language", lang, "--layout", layout, "--match", match,
           "--context", "--dump", path, swipes]
    subprocess.run(cmd, capture_output=True, text=True, check=True)


def grade(rows, wn, wg):
    """(swipes, first right, top-3 right) with these weights."""
    first = top3 = 0
    for r in rows:
        c = r["c"]
        if not c:
            continue
        if r["before"]:
            # WHY sorted with the index as tie-break: recognize() re-sorts stably, so equal scores keep the search's order.
            order = sorted(range(len(c)), key=lambda k: (-(c[k][0] + (wg - 1) * c[k][1] + wn * LN10 * c[k][2]), k))
        else:
            order = list(range(len(c)))
        first += c[order[0]][3]
        top3 += any(c[k][3] for k in order[:3])
    return len(rows), first, top3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dumps", default=None)
    ap.add_argument("--bench", default=f"{ROOT}/build/tools/swipekb-rescore-bench")
    ap.add_argument("-j", type=int, default=3)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    dumps = a.dumps or os.path.join(ROOT, "build", "rescore-dumps")
    os.makedirs(dumps, exist_ok=True)
    paths = {n: os.path.join(dumps, f"dump-{n}.jsonl") for n in SETS}
    with cf.ThreadPoolExecutor(a.j) as ex:
        list(ex.map(lambda n: make_dump(a.bench, n, paths[n]), [n for n in SETS if not os.path.exists(paths[n])]))
    data = {n: [json.loads(l) for l in open(paths[n])] for n in SETS}
    result = {}
    for group, names in GROUPS.items():
        rows = [r for n in names for r in data[n]]
        # The swipes' own first-guess rights as recorded must equal the neutral grid point (a check on the replay).
        n0, f0, t0 = grade(rows, 0, 1)
        recorded = sum(r["right"] for r in rows)
        grid = {(wn, wg): grade(rows, wn, wg) for wn in NEURAL for wg in NGRAM}
        best = max(grid, key=lambda k: (grid[k][1], -abs(k[0]) - abs(k[1] - 1)))
        print(f"== {group} ({'+'.join(names)}): {n0} swipes, {sum(r['before'] for r in rows)} with words before; "
              f"first right today {f0} (recorded {recorded}), top-3 of the list {t0}")
        print("   first-right counts, rows = neural weight, columns = n-gram weight " + " ".join(f"{g:>6}" for g in NGRAM))
        for wn in NEURAL:
            print(f"   {wn:>5} " + " ".join(f"{grid[(wn, g)][1]:>6}" for g in NGRAM))
        bn, bf, bt = grid[best]
        print(f"   best: neural {best[0]} ngram {best[1]}: first {100 * bf / bn:.2f}% ({bf - f0:+d} swipes) "
              f"top-3 of the list {100 * bt / bn:.2f}% ({bt - t0:+d}); today first {100 * f0 / n0:.2f}% top-3 {100 * t0 / n0:.2f}%")
        result[group] = {"neural": best[0], "ngram": best[1], "swipes": n0, "first_today": f0, "first_best": bf,
                         "top3_today": t0, "top3_best": bt}
    if a.out:
        with open(a.out, "w") as f:
            json.dump(result, f, indent=1)
    print(json.dumps(result))


if __name__ == "__main__":
    sys.exit(main())
