# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Evaluation-set converter: {"cases":[{target, keys[x,y,w,h px], points[x,y,t px]}]} json -> swipe jsonl + layout tsv.

  python -I convert_cases.py synthetic-noisy-v1-es.json out-prefix   ->  out-prefix.jsonl, out-prefix.layout.tsv
The letter area is the bounding box of the keys; x, y become fractions of it (the form the swipe tools read).
Test data only: never feed the output to training.
"""
import json
import sys

d = json.load(open(sys.argv[1], encoding="utf-8"))
cases = d["cases"]
keys = cases[0]["keys"]
W = max(k["x"] + k["w"] for k in keys)
H = max(k["y"] + k["h"] for k in keys)
with open(sys.argv[2] + ".layout.tsv", "w", encoding="utf-8") as f:
    f.write("letter\tcx\tcy\thalf_w\thalf_h\n")
    for k in keys:
        f.write(f"{k['letter']}\t{(k['x'] + k['w'] / 2) / W:.5f}\t{(k['y'] + k['h'] / 2) / H:.5f}\t{k['w'] / 2 / W:.5f}\t{k['h'] / 2 / H:.5f}\n")
n = 0
with open(sys.argv[2] + ".jsonl", "w", encoding="utf-8") as f:
    for c in cases:
        pts = c.get("points")
        if not pts or len(pts) < 2:
            continue
        f.write(json.dumps({"id": c["id"], "word": c["target"], "canvas_width": W, "canvas_height": H,
                            "data": [{"t": p["t"], "x": p["x"] / W, "y": p["y"] / H} for p in pts]}, ensure_ascii=False) + "\n")
        n += 1
print(f"{n} of {len(cases)} cases, area {W:.1f} x {H:.1f}, {len(keys)} keys")
