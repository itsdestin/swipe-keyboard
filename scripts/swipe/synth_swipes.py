# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Our own synthetic swipe generator (training data for languages without real swipes, and extra English).

  python -I synth_swipes.py --words words.tsv --layout layouts/azerty.tsv --n 200000 --out synth-fr.jsonl

A person swipes a smooth curve through (roughly) the letters, faster on straight parts and slower at turns, with
sloppy letter positions, sometimes cutting a corner or skipping a middle letter, lifting early or overshooting at the
end, and pausing on a double letter. All noise sizes are drawn per swipe from broad ranges, so the model sees
careful and sloppy swipers. Output is the same jsonl format as the real recordings.

WHY a separate generator from bench/languages/make-noisy-swipes.py: models trained on one generator must be scored
on swipes from another (or real ones), or the score only measures how well the model learned the generator.
Written from scratch; constants are chosen by eye from the published literature on touch-typing error sizes and
checked only against the real English recordings' summary statistics (start error ~0.27 key, duration ~0.8 s).
"""
import argparse
import json
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402


def catmull_rom(pts, per_seg=24, tension=0.5):
    """Smooth curve through pts (n,2) (centripetal-ish Catmull-Rom with end points repeated)."""
    if len(pts) == 1:
        return pts.copy()
    p = np.vstack([pts[0], pts, pts[-1]])
    out = []
    for i in range(1, len(p) - 2):
        p0, p1, p2, p3 = p[i - 1], p[i], p[i + 1], p[i + 2]
        t = np.linspace(0, 1, per_seg, endpoint=False)[:, None]
        m1 = tension * (p2 - p0)
        m2 = tension * (p3 - p1)
        out.append((2 * t ** 3 - 3 * t ** 2 + 1) * p1 + (t ** 3 - 2 * t ** 2 + t) * m1
                   + (-2 * t ** 3 + 3 * t ** 2) * p2 + (t ** 3 - t ** 2) * m2)
    out.append(pts[-1][None])
    return np.vstack(out)


def make_swipe(rng, keys_xy, seq, doubles, unit, aspect):
    """keys_xy: (K,2) key centres in key-width units; seq: collapsed key indices; doubles: set of positions in seq."""
    sloppy = rng.lognormal(0.0, 0.45)                       # per-swipe care level
    sx, sy = 0.16 * sloppy, 0.22 * sloppy                   # letter position noise (key widths)
    bias = rng.normal(0, 0.12, size=2) + np.array([0.0, 0.1])   # whole-swipe offset (thumbs land low)
    way = keys_xy[seq] + rng.normal(0, 1, size=(len(seq), 2)) * [sx, sy] + bias
    # optional: skip a middle letter / cut a corner
    if len(seq) > 3 and rng.random() < 0.04:
        j = rng.integers(1, len(seq) - 1)
        way[j] = (way[j - 1] + way[j + 1]) / 2 + rng.normal(0, 0.3, 2)
    # start is accurate, end is looser and may be early / over
    way[0] = keys_xy[seq[0]] + rng.normal(0, 1, 2) * [0.2, 0.22] * sloppy + bias
    way[-1] = way[-1] + rng.normal(0, 1, 2) * [0.35, 0.25]
    curve = catmull_rom(way, tension=rng.uniform(0.3, 0.7))
    seg = np.linalg.norm(np.diff(curve, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    L = s[-1]
    if L < 1e-6:   # single key: a tap with a little slide
        n = int(rng.integers(2, 6)); t = np.cumsum(rng.uniform(0.010, 0.025, n))
        pts = way[0] + rng.normal(0, 0.05, (n, 2))
        return pts, t
    # speed: base speed (key widths per second), slower at turns (curvature) and at the ends
    base = rng.lognormal(math.log(19.0), 0.45)
    ang = np.zeros(len(curve))
    d = np.diff(curve, axis=0)
    for i in range(1, len(d)):
        a = math.atan2(d[i][1], d[i][0]) - math.atan2(d[i - 1][1], d[i - 1][0])
        ang[i] = abs((a + math.pi) % (2 * math.pi) - math.pi)
    ang = np.convolve(ang, np.ones(7) / 7, mode="same")
    v = base * (1.0 / (1.0 + 1.2 * ang))
    ramp = np.minimum(1.0, 0.35 + 0.65 * np.minimum(s, L - s) / max(1e-6, min(1.5, L / 2)))
    v = np.maximum(v * ramp, 1.5)
    dt = np.concatenate([[0], seg / v[1:]])
    t = np.cumsum(dt)
    # pauses on doubled letters: find the curve index nearest each waypoint
    pause = np.zeros(len(curve))
    wp_idx = np.linspace(0, len(curve) - 1, len(seq)).astype(int)
    for pos in doubles:
        pause[wp_idx[pos]] += rng.uniform(0.05, 0.14)
    t = t + np.cumsum(pause)
    # sample at ~60 Hz with jitter
    total = t[-1]
    ts = [0.0]
    while ts[-1] < total:
        ts.append(ts[-1] + rng.uniform(0.010, 0.024))
    ts = np.array(ts[:-1] + [total])
    px = np.interp(ts, t, curve[:, 0]) + rng.normal(0, 0.02, len(ts))
    py = np.interp(ts, t, curve[:, 1]) + rng.normal(0, 0.02, len(ts))
    return np.stack([px, py], 1), ts * 1000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", required=True, help="word<TAB>freq")
    ap.add_argument("--layout", required=True)
    ap.add_argument("--n", type=int, default=100000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--min-freq", type=int, default=40)
    ap.add_argument("--freq-div", type=float, nargs="+", default=[8.0, 16.0], help="word sampling sharpness; one distribution per value, used alternately")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    layout = S.Layout(a.layout)
    words, freqs = [], []
    for line in open(a.words, encoding="utf-8"):
        w, _, f = line.rstrip("\n").partition("\t")
        if not f.isdigit() or int(f) < a.min_freq:
            continue
        keys = layout.word_keys(w)
        if not keys or len(keys) > 20:
            continue
        words.append(w); freqs.append(int(f))
    # WHY two mixed distributions: people mostly swipe common short words (mean ~5 letters in the real recordings) but
    # the model also needs many rare, longer words; half the swipes follow a sharp frequency curve, half a flat one.
    freqs = np.array(freqs, float)
    weights = []
    for div in a.freq_div:
        w_ = 2.0 ** (freqs / div); weights.append(w_ / w_.sum())
    with open(a.out, "w", encoding="utf-8") as out:
        for i in range(a.n):
            w = words[rng.choice(len(words), p=weights[i % len(weights)])]
            W = float(rng.integers(340, 440)); H = 170.3125
            aspect = H / W
            keys_xy = layout.key_array(aspect)[:, :2]
            seq = layout.word_keys(w)
            raw = [k for ch in w if (k := layout.fold(ch)) >= 0]
            # positions in `seq` whose letter was doubled in the raw spelling
            dbl, idx = set(), -1
            for j, k in enumerate(raw):
                if j == 0 or raw[j - 1] != k:
                    idx += 1
                else:
                    dbl.add(idx)
            pts, ts = make_swipe(rng, keys_xy, seq, dbl, layout.unit, aspect)
            t0 = 1.7e12 + i * 5000.0
            data = [{"t": float(t0 + t), "x": float(p[0] / layout.unit), "y": float(p[1] / (layout.unit * aspect))}
                    for p, t in zip(pts, ts)]
            out.write(json.dumps({"id": i, "session": "synth", "word": w, "canvas_width": W, "canvas_height": H, "data": data}) + "\n")


if __name__ == "__main__":
    main()
