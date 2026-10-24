"""Add up swipekb-language-bench outputs for HeliBoard test chunks (python -I heliboard_score_summary.py <dir with *-test-NN.out>).

Prints, per language and per layout type: cases, evaluated, first guess / top 3 case-insensitive (the bench's lookupTop1/3,
case folded, accents kept) and case-sensitive, and how many targets are in the pack dictionary. Denominator = evaluated
cases (invalid = geometry the bench refused; counted separately).
"""
import collections
import glob
import json
import os
import sys

d = sys.argv[1]
agg = collections.defaultdict(lambda: collections.Counter())
for p in sorted(glob.glob(os.path.join(d, "*-test-*.out"))):
    lang = os.path.basename(p).split("-")[0]
    for c in json.load(open(p))["cases"]:
        for key in (lang, f"{lang}:{c['layout']}"):
            a = agg[key]; a["cases"] += 1
            if c["status"] != "evaluated":
                a[c["status"]] += 1
                continue
            a["evaluated"] += 1
            for f in ("top1", "top3", "lookupTop1", "lookupTop3", "targetInDictionary"):
                a[f] += bool(c[f])
            # accent-blind first guess, to separate "wrong word" from "right word, accent missing"
            import unicodedata
            fold = lambda s: "".join(ch for ch in unicodedata.normalize("NFD", s.lower()) if not unicodedata.combining(ch))
            cand = c["candidates"]
            a["accentblind1"] += bool(cand) and fold(cand[0]) == fold(c["target"])
for k in sorted(agg):
    a = agg[k]; n = a["evaluated"] or 1
    print(f"{k:14} cases {a['cases']:5} evaluated {a['evaluated']:5} invalid {a['invalid']:3} | first {100*a['lookupTop1']/n:5.1f}% top3 {100*a['lookupTop3']/n:5.1f}% "
          f"| case-sens first {100*a['top1']/n:5.1f}% top3 {100*a['top3']/n:5.1f}% | accent-blind first {100*a['accentblind1']/n:5.1f}% | in-dict {100*a['targetInDictionary']/n:5.1f}%")
