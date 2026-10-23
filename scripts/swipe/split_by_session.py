"""Split a swipe .jsonl into train / validation by whole recording session (python -I split_by_session.py in.jsonl out-prefix).

WHY by session: swipes from one person are alike; a random split would leak that person into validation. Also note
(2026-10-23): devhold-5000 and tune-*.jsonl are subsets of dev.jsonl, so they are NOT held-out data.
"""
import hashlib
import sys

n_train = n_val = 0
with open(sys.argv[1], encoding="utf-8") as f, open(sys.argv[2] + "-train.jsonl", "w", encoding="utf-8") as ft, \
        open(sys.argv[2] + "-val.jsonl", "w", encoding="utf-8") as fv:
    import json
    for line in f:
        s = json.loads(line)["session"]
        # stable hash (not Python's randomised hash()) so the split is reproducible
        bucket = int(hashlib.sha1(s.encode()).hexdigest(), 16) % 10
        if bucket == 0:
            fv.write(line); n_val += 1
        else:
            ft.write(line); n_train += 1
print(f"train {n_train}  val {n_val}")
