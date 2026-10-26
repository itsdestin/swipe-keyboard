#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""bench/swap-tune.py — tunes the swap list's weights (SwapParams, card #500) on TUNING data only.

Coordinate search: start with every weight at 1; each round grades every one-weight change from the current point
(each weight in {0, .25, .5, 1, 2, 4}) in a single pass of `swipekb-swap-bench --mode new --params <array>`, takes
the best change, and stops when no change improves. Done separately for each pool size {16, 24, 32}; the best wins.

Objective: the mean of the three "right word among the options" (in3) rates for swipes whose first guess was wrong
(after-swipe, tap-back with the swipe's guesses, tap-back without them). Hard reject: junk in the bar after a swipe
(measure 4) more than 1 point above today's keyboard (--mode baseline) on the same swipes, or junk among the tap-back
options (measure 4b, with or without the swipe's guesses) more than 3 points above it (the card #500 limits; before
2026-10-25 only the first was enforced here). Several --set are pooled (counts added up) for the objective, but the
junk limits hold for EACH set on its own (WHY, 2026-10-25: pooled, the 7,198 English swipes outweighed French and
German, so weights that raised their junk by 10 points still passed).

Usage: bench/swap-tune.py --set trv8k [--set ...] [-j 3] [--bench PATH] [--log FILE] [--out FILE]
                          [--fixed JSON] [--pools 16,24,32] [--weights wPath,wBefore,wAfter,wNeural] [--start JSON]
  --fixed SwapParams keys held fixed during the search, e.g. the gathering settings '{"beam": 256, "neighbours": 12}'
          (WHY: they change which words are found, so the weights are re-tuned on top of each structural change).
  --pools the pool sizes searched (each gets its own coordinate search).
  --weights the weights searched (default the four sentence weights). The no-sentence weights are
          ncPath,ncBefore,ncNeural, tuned on the -nocontext sets (WHY: they rank only lists with no words around the
          word, so only those swipes can say what they should be).
  --start the search's starting values for --weights (default 1 each), e.g. '{"ncPath": 1, "ncBefore": 0, "ncNeural": 0}'.
  --set   trv8k (FUTO English tuning slice) | tune-fr | tune-de (FUTO swipe-5 validation sessions, AZERTY French and
          German layout), or the same with "-nocontext" (trv8k-nocontext, ...): the swipes with their sentence
          removed (swipekb-swap-bench --no-context), as a HeliBoard record or the first word of a message looks.
          Anything else is refused. WHY: the test sets (bench-2000, test-extra-10000, HeliBoard) are
          never tuned on, so a tuned number on them stays an honest result.
Env: SWIPEKB_ASSETS (language files in its language-files/), SWIPEKB_TUNE_DIR (~/.cache/swipe-keyboard-train/shape/futo).
Output: one JSON line per graded set of weights in --log, and the winner (weights, objective, every measure next to
today's keyboard) as JSON in --out and on stdout.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNE = os.environ.get("SWIPEKB_TUNE_DIR", os.path.expanduser("~/.cache/swipe-keyboard-train/shape/futo"))
LANG_FILES = os.path.join(os.environ.get("SWIPEKB_ASSETS", os.path.join(ROOT, "assets")), "language-files")
# The only sets this script will tune on: swipes, language, locale, keys, word comparison.
SETS = {
    "trv8k": (f"{TUNE}/trv8k.jsonl", "en", "en_US", f"{ROOT}/scripts/swipe/layouts/qwerty.tsv", "ascii"),
    "tune-fr": (f"{TUNE}/s5-azerty-val.jsonl", "fr", "fr_FR", f"{TUNE}/layouts/azerty.tsv", "lookup"),
    "tune-de": (f"{TUNE}/s5-german-val.jsonl", "de", "de_DE", f"{TUNE}/layouts/german.tsv", "lookup"),
}
# WHY the same swipes without their sentence (2026-10-25): the weights above were tuned only with sentences, and on
# swipes with none (HeliBoard, the first word of a message) the lists came out worse than today's keyboard; these sets
# measure exactly that case on tuning data.
SETS.update({f"{k}-nocontext": v for k, v in list(SETS.items())})
VALUES = [0, 0.25, 0.5, 1, 2, 4]
POOLS = [16, 24, 32]
WEIGHTS = ["wPath", "wBefore", "wAfter", "wNeural"]
MEASURES = ["after-swipe", "tapback-trace", "tapback-notrace"]
LINE = re.compile(r"^(after-swipe|tapback-trace|tapback-notrace): cases (\d+) first ([\d.]+)% in3 ([\d.]+)%")
# WHY the raw counts ("count N", "counts N M"): pooling several sets adds counts, and a count worked back from a
# rounded percentage can be off once thousands of options are shown.
JUNK = re.compile(r"^junk: right (\d+) shown (\d+) junk [\d.]+% count (\d+)")
TAP_JUNK = re.compile(r"^junk-tapback: trace shown (\d+) junk [\d.]+% notrace shown (\d+) junk [\d.]+% counts (\d+) (\d+)")
SPEED = re.compile(r"^speed: ([\d.]+) ms/list")


def run_bench(bench, set_name, mode, params):
    swipes, lang, locale, layout, match = SETS[set_name]
    no_context = ["--no-context"] if set_name.endswith("-nocontext") else []
    dict_file = f"{LANG_FILES}/{'en_swipe' if lang == 'en' else lang}.combined"
    cmd = ["nice", "-n", "10", bench, "--mode", mode, "--dict", dict_file, "--model", f"{LANG_FILES}/swipe-model.bin",
           "--ngram", f"{LANG_FILES}/{lang}.ngram", "--vocab", f"{LANG_FILES}/{lang}.vocab.tsv",
           "--nwp", f"{LANG_FILES}/{lang}.nwp.bin", "--language", lang, "--locale", locale, "--layout", layout,
           "--match", match, "--context"] + no_context
    tmp = None
    if params is not None:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(params, tmp)
        tmp.close()
        cmd += ["--params", tmp.name]
    try:
        out = subprocess.run(cmd + [swipes], capture_output=True, text=True, check=True).stdout
    finally:
        if tmp:
            os.unlink(tmp.name)
    # Blocks: one per parameter set ("== params N" before each when there are several).
    blocks, cur = [], None
    for line in out.splitlines():
        if line.startswith("== params") or cur is None and (LINE.match(line) or JUNK.match(line)):
            cur = {}
            blocks.append(cur)
            if line.startswith("=="):
                continue
        if m := LINE.match(line):
            cases = int(m[2])
            # Counts back from the printed percentages (2 decimals is exact enough for whole counts).
            cur[m[1]] = (cases, round(float(m[3]) * cases / 100), round(float(m[4]) * cases / 100))
        elif m := JUNK.match(line):
            cur["junk"] = (int(m[2]), int(m[3]))
        elif m := TAP_JUNK.match(line):
            cur["tap-junk"] = ((int(m[1]), int(m[3])), (int(m[2]), int(m[4])))
        elif m := SPEED.match(line):
            cur["ms"] = float(m[1])
    return blocks


def pooled(results):
    """Several sets' blocks for one parameter set -> rates (percent) over all their swipes."""
    r = {}
    for k in MEASURES:
        cases = sum(x[k][0] for x in results)
        r[k] = {"cases": cases, "first": 100 * sum(x[k][1] for x in results) / max(cases, 1),
                "in3": 100 * sum(x[k][2] for x in results) / max(cases, 1)}
    shown = sum(x["junk"][0] for x in results)
    r["junk"] = 100 * sum(x["junk"][1] for x in results) / max(shown, 1)
    # Measure 4b, reported next to the rest (today's keyboard has no tap-back rare-word rule, so no baseline to reject on).
    for i, name in enumerate(["tap-junk-trace", "tap-junk-notrace"]):
        tshown = sum(x["tap-junk"][i][0] for x in results if "tap-junk" in x)
        r[name] = 100 * sum(x["tap-junk"][i][1] for x in results if "tap-junk" in x) / max(tshown, 1)
    r["ms"] = max(x.get("ms", 0) for x in results)
    r["objective"] = sum(r[k]["in3"] for k in MEASURES) / 3
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", action="append", required=True)
    ap.add_argument("-j", type=int, default=3)
    ap.add_argument("--bench", default=os.path.join(ROOT, "build", "tools", "swipekb-swap-bench"))
    ap.add_argument("--log", default=None)
    ap.add_argument("--out", default=None)
    ap.add_argument("--fixed", default="{}")
    ap.add_argument("--pools", default=",".join(str(p) for p in POOLS))
    ap.add_argument("--weights", default=",".join(WEIGHTS))
    ap.add_argument("--start", default="{}")
    a = ap.parse_args()
    for s in a.set:
        if s not in SETS:
            sys.exit(f"swap-tune.py: refusing --set {s}: only the tuning sets {sorted(SETS)} may be tuned on "
                     "(the test sets are never tuned on)")
        if not os.path.exists(SETS[s][0]):
            sys.exit(f"swap-tune.py: missing tuning swipes {SETS[s][0]}")
    log = open(a.log, "a") if a.log else None
    fixed = json.loads(a.fixed)
    weights = a.weights.split(",")
    start = json.loads(a.start)
    ex = cf.ThreadPoolExecutor(max_workers=max(1, min(a.j, 4)))   # WHY at most 4: a tablet the owner is using

    def grade(params_list):
        futs = [ex.submit(run_bench, a.bench, s, "new", params_list) for s in a.set]
        per_set = [f.result() for f in futs]
        out = []
        for i, p in enumerate(params_list):
            r = pooled([blocks[i] for blocks in per_set])
            r["per_set"] = {s: pooled([blocks[i]]) for s, blocks in zip(a.set, per_set)}
            r["params"] = p
            out.append(r)
            if log:
                log.write(json.dumps(r) + "\n")
                log.flush()
        return out

    base_blocks = [b[0] for b in ex.map(lambda s: run_bench(a.bench, s, "baseline", None), a.set)]
    base = pooled(base_blocks)
    base["per_set"] = {s: pooled([b]) for s, b in zip(a.set, base_blocks)}
    # Per set: bar junk at most +1 point, tap-back junk at most +3 points over today's keyboard on the same swipes.
    limits = {s: {"junk": b["junk"] + 1.0, "tap-junk-trace": b["tap-junk-trace"] + 3.0,
                  "tap-junk-notrace": b["tap-junk-notrace"] + 3.0} for s, b in base["per_set"].items()}
    print(f"baseline: objective {base['objective']:.2f}", flush=True)
    for s, b in base["per_set"].items():
        print(f"  {s}: objective {b['objective']:.2f} junk {b['junk']:.2f}% tap-back {b['tap-junk-trace']:.2f}% / "
              f"{b['tap-junk-notrace']:.2f}% (reject above {limits[s]['junk']:.2f} / {limits[s]['tap-junk-trace']:.2f} / "
              f"{limits[s]['tap-junk-notrace']:.2f})", flush=True)

    def ok(r):
        return all(r["per_set"][s][k] <= v + 1e-9 for s, lim in limits.items() for k, v in lim.items())

    def search(pool):
        cur = dict(fixed, **{w: start.get(w, 1) for w in weights}, pool=pool)
        best = grade([cur])[0]
        rounds = 0
        while rounds < 12:
            rounds += 1
            moves = [dict(cur, **{w: v}) for w in weights for v in VALUES if v != cur[w]]
            graded = [r for r in grade(moves) if ok(r)]
            top = max(graded, key=lambda r: r["objective"], default=None)
            # WHY strictly better (and the start must pass the junk rule too): a tie keeps the simpler current point.
            if top and (top["objective"] > best["objective"] or not ok(best)):
                cur, best = top["params"], top
                print(f"pool {pool} round {rounds}: {cur} objective {best['objective']:.2f} junk {best['junk']:.2f}%",
                      flush=True)
            else:
                break
        return best

    # WHY one thread per pool (nested work goes to the same capped executor through grade()).
    pools_list = [int(x) for x in a.pools.split(",")]
    with cf.ThreadPoolExecutor(max_workers=len(pools_list)) as pools:
        finals = list(pools.map(search, pools_list))
    winners = [f for f in finals if ok(f)]
    # WHY lower junk breaks a tie: two weight sets that find the right word equally often differ only in how many
    # nonsense words they show.
    win = max(winners, key=lambda r: (r["objective"], -r["junk"])) if winners else None
    result = {"sets": a.set, "fixed": fixed, "baseline": base, "limits": limits, "per_pool": finals, "winner": win}
    text = json.dumps(result, indent=1)
    print(text)
    if a.out:
        with open(a.out, "w") as f:
            f.write(text + "\n")


if __name__ == "__main__":
    main()
