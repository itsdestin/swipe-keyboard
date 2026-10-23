"""Keep a deterministic fraction of whole sessions: python -I subset_by_session.py in.jsonl out.jsonl PERCENT [VAL_EVERY_N_SESSIONS_OUT]

WHY whole sessions: swipes of one person are alike, so a size-matched subset must not cut people in half.
Optional 4th argument: write a second file (out.jsonl.val) with sessions whose hash falls in the next 10% band,
for validation that shares no session with the training subset.
"""
import hashlib
import sys

pct = int(sys.argv[3])
val_out = open(sys.argv[2] + ".val", "w", encoding="utf-8") if len(sys.argv) > 4 else None
n = nv = 0
with open(sys.argv[1], encoding="utf-8") as f, open(sys.argv[2], "w", encoding="utf-8") as o:
    for line in f:
        i = line.index('"session":')
        sess = line[i + 11:line.index('"', i + 11)]
        b = int(hashlib.sha1(sess.encode()).hexdigest(), 16) % 100
        if b < pct:
            o.write(line); n += 1
        elif val_out and pct <= b < pct + 10:
            val_out.write(line); nv += 1
print(f"kept {n}" + (f", val {nv}" if val_out else ""))
