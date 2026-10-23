"""Dictionary file (.combined, ' word=X,f=N,...' lines) -> 'word<TAB>freq' TSV used by the swipe tools.

  python -I words_from_combined.py es.combined > es-words.tsv
"""
import re
import sys

pat = re.compile(r"^\s*word=(.*?),f=(\d+)(?:,|$)")
for line in open(sys.argv[1], encoding="utf-8"):
    m = pat.match(line)
    if m and m.group(1) and "\t" not in m.group(1):
        print(f"{m.group(1)}\t{m.group(2)}")
