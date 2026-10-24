# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Measurement helper: a dictionary the way the keyboard has it AFTER the built-in texting list is merged.

  python -I bench/texting-dict.py DICT.combined packaging/texting-words/en.tsv OUT.combined [OUT-words.tsv]

WHY: the keyboard merges packaging/texting-words/LANG.tsv at start-up (LanguageEngine::liftWords: a word is only
RAISED, a "not a word" mark is lifted). The accuracy benches read a dictionary FILE, so this builds the same result
as a file, to compare swipe accuracy before and after. OUT-words.tsv (optional) is the "word<TAB>freq" list of the
swipe engine (the same acceptance rule as engine/src/language_engine.cpp: score above 0, not "nonword" / not_a_word).
Pass an empty list file name (-) to write the unchanged dictionary (the "before" side).
"""
import re
import sys

pat = re.compile(r"^ word=(.*?),f=(\d+)(.*)$")
dic, lst, out = sys.argv[1], sys.argv[2], sys.argv[3]
words_out = sys.argv[4] if len(sys.argv) > 4 else None
lift = {}
if lst != "-":
    for line in open(lst, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        c = line.rstrip("\n").split("\t")
        if len(c) >= 3 and c[1].isdigit():
            lift[c[0].lower()] = max(lift.get(c[0].lower(), 0), int(c[1]))
lines = open(dic, encoding="utf-8").read().split("\n")
best = {}
for ln in lines:
    m = pat.match(ln)
    if m and "nonword" not in ln and "not_a_word=true" not in ln:
        best[m.group(1).lower()] = max(best.get(m.group(1).lower(), 0), int(m.group(2)))
need = {w for w, s in lift.items() if best.get(w, 0) < s}
kept = []
for ln in lines:
    m = pat.match(ln)
    if m and m.group(1).lower() in need and m.group(1) == m.group(1).lower():
        continue  # replaced below (an old weak or not-a-word line for the same lower-case spelling)
    kept.append(ln)
while kept and kept[-1] == "":
    kept.pop()
for w in sorted(need):
    kept.append(f" word={w},f={lift[w]},flags=,originalFreq={lift[w]}")
open(out, "w", encoding="utf-8").write("\n".join(kept) + "\n")
if words_out:
    with open(words_out, "w", encoding="utf-8") as f:
        for ln in kept:
            m = pat.match(ln)
            if m and int(m.group(2)) > 0 and "nonword" not in ln and "not_a_word=true" not in ln and m.group(1) and "\t" not in m.group(1):
                f.write(f"{m.group(1)}\t{m.group(2)}\n")
print(f"lifted {len(need)} of {len(lift)} listed words", file=sys.stderr)
