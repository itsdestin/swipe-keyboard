# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Writes tests/data/swipe_net/: a tiny random network, one trace and what PyTorch computes for it.

The C++ runner (engine/src/swipe_net.cpp) must reproduce these numbers (tests/features: tst_swipenet). Re-run only
when the model format or the input features change:  python -I scripts/swipe/make_fixture.py
"""
import copy
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
out = os.path.join(root, "tests", "data", "swipe_net")
os.makedirs(out, exist_ok=True)
layout = S.Layout(os.path.join(root, "scripts", "swipe", "layouts", "qwerty.tsv"))
torch.manual_seed(7)
cfg = dict(conv=[8, 6], kernel=5, hidden=6, layers=2, pointer=5)
net = S.build_net(cfg)
with torch.no_grad():   # larger weights so every gate and the tanh pointer are exercised, not near zero
    for p in net.parameters():
        p.mul_(3.0)
net.eval()

# A trace: "hello"-like path with a pause, canvas 400x160, irregular sample times, one stationary stretch.
rng = np.random.default_rng(3)
word = [layout.index[c] for c in "hello"]
pts = []
t = 1000.0
for k in word:
    cx, cy = layout.geom[k][0], layout.geom[k][1]
    for _ in range(4):
        pts.append((cx + rng.normal(0, 0.015), cy + rng.normal(0, 0.03), t))
        t += float(rng.integers(8, 30))
pts.insert(9, (pts[8][0], pts[8][1], pts[8][2] + 40))   # dwell: same place, 40 ms later
aspect = 160 / 400


def run(model, tag):
    feats, pos = S.trace_to_input([p[0] for p in pts], [p[1] for p in pts], [p[2] for p in pts], aspect, layout)
    keys = layout.key_array(aspect).astype(np.float32)
    with torch.no_grad():
        lp = model(torch.from_numpy(feats)[None], torch.from_numpy(pos)[None], torch.from_numpy(keys)[None])[0].numpy()
    return feats, pos, keys, lp


feats, pos, keys, lp = run(net, "f32")
S.export_model(net, os.path.join(out, "model_f32.bin"), "f32")
# f16 variant: round the weights the way the file does, then ask PyTorch what that rounded net computes.
net16 = copy.deepcopy(net)
with torch.no_grad():
    for p in net16.parameters():
        p.copy_(p.half().float())
S.export_model(net16, os.path.join(out, "model_f16.bin"), "f16")
_, _, _, lp16 = run(net16, "f16")
json.dump({
    "aspect": aspect,
    "points": [{"x": p[0], "y": p[1], "t": p[2]} for p in pts],
    "feats": feats.flatten().tolist(), "pos": pos.flatten().tolist(), "keys": keys.flatten().tolist(),
    "logp_f32": lp.flatten().tolist(), "logp_f16": lp16.flatten().tolist(),
    "key_count": len(layout.labels), "steps": S.T_STEPS,
}, open(os.path.join(out, "case.json"), "w"))
print("wrote", out, "max|logp|", float(np.abs(lp).max()))
