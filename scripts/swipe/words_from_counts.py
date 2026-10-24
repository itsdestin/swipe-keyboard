# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Big word list from corpus counts: 'word<TAB>count<TAB>docs<TAB>display' -> 'display<TAB>freq' (freq 0..255).

  python -I words_from_counts.py es.wordcounts.tsv es-words.tsv 250000 > es-words-250k.tsv
The count -> freq mapping is fitted (least squares on log2 count) to the existing 100k list so both share one scale.
Only letters and inner apostrophes/hyphens are kept in the spelling test; hyphens/digits-only entries are dropped.
"""
import math
import re
import sys

counts_path, ref_path, top = sys.argv[1], sys.argv[2], int(sys.argv[3])
ref = {}
for line in open(ref_path, encoding="utf-8"):
    w, _, f = line.rstrip("\n").partition("\t")
    ref[w.lower()] = int(f)
rows = []
for line in open(counts_path, encoding="utf-8"):
    if line.startswith("#"):
        continue
    p = line.rstrip("\n").split("\t")
    if len(p) < 2 or not p[1].isdigit():
        continue
    disp = p[3] if len(p) > 3 and p[3] else p[0]
    if not re.fullmatch(r"[^\W\d_]+(['’][^\W\d_]+)*", disp):
        continue
    rows.append((disp, int(p[1])))
    if len(rows) >= top:
        break
xs, ys = [], []
for w, c in rows:
    if w.lower() in ref:
        xs.append(math.log2(c)); ys.append(ref[w.lower()])
n = len(xs)
mx, my = sum(xs) / n, sum(ys) / n
slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
icpt = my - slope * mx
print(f"# fitted f = {slope:.2f}*log2(count) + {icpt:.1f} on {n} shared words", file=sys.stderr)
for w, c in rows:
    print(f"{w}\t{max(0, min(255, round(slope * math.log2(c) + icpt)))}")
