"""make-slice.py SRC N DEST — write N lines spread evenly across SRC (every k-th line) to DEST.
WHY evenly spread: test.jsonl is grouped by contributor session, so the first N lines would grade
only a handful of people. Deterministic, so every scoreboard run sees the same swipes."""
import sys

src, n, dest = sys.argv[1], int(sys.argv[2]), sys.argv[3]
with open(src, encoding="utf-8") as f:
    lines = f.readlines()
step = max(1, len(lines) // n)
picked = lines[::step][:n]
with open(dest + ".part", "w", encoding="utf-8") as f:
    f.writelines(picked)
import os
os.replace(dest + ".part", dest)
