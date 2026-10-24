# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Collect (session, timestamp) keys and sessions of evaluation files so training data can be checked against them.

  python -I holdout_ids.py out.json file1.jsonl file2.jsonl ...      (reads only the 'session' and 'timestamp' fields)
"""
import json
import sys

keys, sessions = set(), set()
for p in sys.argv[2:]:
    with open(p, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            keys.add(f"{r['session']}|{r['timestamp']}")
            sessions.add(r["session"])
json.dump({"keys": sorted(keys), "sessions": sorted(sessions)}, open(sys.argv[1], "w"))
print(len(keys), "swipes,", len(sessions), "sessions")
