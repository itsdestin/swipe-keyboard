#!/usr/bin/env python3
# tatoeba-credits.py LANG3 LANG3_sentences_detailed.tsv.bz2 OUT.txt
# WHY: CC BY 2.0 FR asks that authors be credited; the web+Tatoeba builder does not write this list itself.
# Tatoeba authors of the sentences a pack trained on (training tenths: id % 10 != 0), for CC BY 2.0 FR credit.
import bz2, sys
lang3, detailed, out = sys.argv[1:]
names = set()
with bz2.open(detailed, "rt", encoding="utf-8") as f:
    for line in f:
        x = line.rstrip("\n").split("\t")
        if len(x) >= 4 and x[1] == lang3 and x[0].isdigit() and int(x[0]) % 10 and x[3] not in ("", "\\N"):
            names.add(x[3])
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("Tatoeba sentence contributors whose CC BY 2.0 FR sentences trained this model:\n")
    for n in sorted(names, key=str.casefold): f.write(n + "\n")
print(lang3, len(names))
