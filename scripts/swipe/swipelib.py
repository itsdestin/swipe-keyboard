"""Shared pieces of the swipe-net pipeline: layout, trace -> model input, labels, the network, model export.

Written for this project without reference to other swipe engines' code. The C++ runner (engine/src/swipe_net.cpp, swipe_trace.cpp) must reproduce
`trace_to_input` and `SwipeNet.forward` exactly; tests/features compares them on a committed fixture.

Method (all first-principles / published ideas): resample the finger trace to a fixed number of steps by arc length,
describe each step by position, motion and the time spent, run a small conv + bidirectional-GRU encoder, and let each
step score every key by a small shared "pointer" network that sees where the finger is relative to that key (so the
key list can be any layout), plus a blank. CTC training (Graves et al. 2006) on the key sequence of the word.
"""
import json
import math
import unicodedata

import numpy as np

T_STEPS = 64          # fixed number of time steps the encoder sees
F_IN = 8              # per-step input features
KEY_FEATS = 4         # per (step, key) pointer features


class Layout:
    """Keys as centre + half size in 0..1 of the letter area (keys.tsv format: letter cx cy half_w half_h)."""

    def __init__(self, path):
        rows = [l.rstrip("\n").split("\t") for l in open(path, encoding="utf-8")][1:]
        self.labels = [r[0] for r in rows if r]
        self.geom = np.array([[float(v) for v in r[1:5]] for r in rows if r], dtype=np.float64)
        self.index = {c: i for i, c in enumerate(self.labels)}
        # WHY unit = key pitch (median distance to the nearest other key centre), not the drawn key width: real boards
        # leave gaps between keys, so the drawn width understates the distance a finger travels (20% on a common
        # HeliBoard layout). Same rule in C++ (SwipeLayout).
        c = self.geom[:, :2]
        d = np.sqrt(((c[:, None, :] - c[None, :, :]) ** 2).sum(-1)) + np.eye(len(c)) * 1e9
        self.unit = 1.0 / max(1e-3, float(np.median(d.min(1))))

    def fold(self, ch):
        """Key index for a character, or -1. Accents fold to their base key unless the layout has the accent."""
        c = ch.lower()
        if c in self.index:
            return self.index[c]
        # Letters that do not decompose (ae, oe, o-slash, sharp s) fold the same way the C++ runner does.
        base = {"\u00e6": "a", "\u0153": "o", "\u00f8": "o", "\u00df": "s"}.get(c) or unicodedata.normalize("NFD", c)[0]
        return self.index.get(base, -1)

    def word_keys(self, word):
        """Collapsed key sequence of a word (repeated keys merged: a swipe passes a double letter once)."""
        out = []
        for ch in word:
            k = self.fold(ch)
            if k >= 0 and (not out or out[-1] != k):
                out.append(k)
        return out

    def key_array(self, aspect):
        """(K,4): centre x, centre y, width, height in key-width units for a canvas with height/width = aspect."""
        u = self.unit
        g = self.geom
        return np.stack([g[:, 0] * u, g[:, 1] * u * aspect, 2 * g[:, 2] * u, 2 * g[:, 3] * u * aspect], 1)


def _lower_bound(a, v):
    lo, hi = 0, len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if a[mid] < v:
            lo = mid + 1
        else:
            hi = mid
    return lo


def trace_to_input(xs, ys, ts, aspect, layout, steps=T_STEPS):
    """Raw trace (x,y in 0..1 layout coords, t in ms) -> (feats[steps,F_IN], pos[steps,2]) as float32.

    Resampling is by arc length with the elapsed time carried along, so a long pause shows up as one step with a big
    dt (a "dwell" cue) instead of many identical steps. Reproduced exactly in C++ (swipe_trace.cpp).
    """
    u = layout.unit
    X = [float(v) * u for v in xs]
    Y = [float(v) * u * aspect for v in ys]
    t0 = float(ts[0])
    TT = [float(v) - t0 for v in ts]
    n = len(X)
    s = [0.0] * n
    for i in range(1, n):
        s[i] = s[i - 1] + math.hypot(X[i] - X[i - 1], Y[i] - Y[i - 1])
    L = s[-1]
    px, py, pt = [], [], []
    for j in range(steps):
        if L < 1e-6:
            px.append(X[0]); py.append(Y[0]); pt.append(TT[-1] * j / (steps - 1))
            continue
        target = L * j / (steps - 1)
        i = _lower_bound(s, target)
        if i <= 0:
            px.append(X[0]); py.append(Y[0]); pt.append(TT[0]); continue
        if i >= n:
            i = n - 1
        d = s[i] - s[i - 1]
        f = (target - s[i - 1]) / d if d > 0 else 1.0
        px.append(X[i - 1] + f * (X[i] - X[i - 1]))
        py.append(Y[i - 1] + f * (Y[i] - Y[i - 1]))
        pt.append(TT[i - 1] + f * (TT[i] - TT[i - 1]))
    feats = np.zeros((steps, F_IN), dtype=np.float64)
    glob = math.log1p(L) / 3.0
    for j in range(steps):
        dx = px[j] - px[j - 1] if j else 0.0
        dy = py[j] - py[j - 1] if j else 0.0
        dt = pt[j] - pt[j - 1] if j else 0.0
        feats[j] = [
            px[j] / u,                                               # x in 0..1 of the letter area
            py[j] / (u * aspect),                                    # y in 0..1 of the letter area
            dx, dy,
            math.log1p(max(dt, 0.0)) / 4.0,
            (j / (steps - 1)) if L >= 1e-6 else 0.0,
            math.hypot(dx, dy) / 2.0,
            glob,
        ]
    pos = np.stack([px, py], 1)
    return feats.astype(np.float32), pos.astype(np.float32)


# ---------------------------------------------------------------- network
def build_net(cfg=None, dropout=0.0):
    import torch
    from torch import nn

    cfg = {**dict(conv=[64, 64], kernel=5, hidden=96, layers=2, pointer=32), **(cfg or {})}

    class SwipeNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.cfg = cfg
            chans = [F_IN] + cfg["conv"]
            self.convs = nn.ModuleList(
                nn.Conv1d(chans[i], chans[i + 1], cfg["kernel"], padding=cfg["kernel"] // 2) for i in range(len(cfg["conv"])))
            # Dropout exists only while training (nothing to export): it fights memorising the few real swipes.
            self.drop = nn.Dropout(dropout)
            self.gru = nn.GRU(chans[-1], cfg["hidden"], num_layers=cfg["layers"], bidirectional=True, batch_first=True,
                              dropout=dropout if cfg["layers"] > 1 else 0.0)
            H2 = 2 * cfg["hidden"]
            P = cfg["pointer"]
            self.A = nn.Linear(H2, P)               # step -> pointer space
            self.B = nn.Linear(KEY_FEATS, P)        # (step, key) geometry -> pointer space
            self.w = nn.Linear(P, 1)                # pointer -> key logit
            self.blank = nn.Linear(H2, 1)

        def forward(self, feats, pos, keys, key_mask=None):
            """feats (B,T,F_IN), pos (B,T,2), keys (B,K,4) -> log-probs (B,T,1+K) with blank at index 0.

            key_mask (B,K) bool: False marks padding keys (batches mixing layouts of different sizes); they can
            never be output."""
            h = feats.transpose(1, 2)
            for c in self.convs:
                h = self.drop(torch.relu(c(h)))
            h, _ = self.gru(h.transpose(1, 2))
            h = self.drop(h)
            a = self.A(h)                                                    # (B,T,P)
            dx = pos[:, :, None, 0] - keys[:, None, :, 0]                    # (B,T,K) key-width units
            dy = pos[:, :, None, 1] - keys[:, None, :, 1]
            kw = keys[:, None, :, 2].expand_as(dx)
            kh = keys[:, None, :, 3].expand_as(dx)
            f = torch.stack([dx, dy, kw, kh], -1)                            # (B,T,K,4)
            logit = self.w(torch.tanh(a[:, :, None, :] + self.B(f))).squeeze(-1)   # (B,T,K)
            if key_mask is not None:
                logit = logit.masked_fill(~key_mask[:, None, :], -1e4)
            blank = self.blank(h)                                            # (B,T,1)
            return torch.log_softmax(torch.cat([blank, logit], -1), -1)

    return SwipeNet()


# ---------------------------------------------------------------- export (format "SKNN" v1, little endian)
def export_model(net, path, dtype="f32"):
    """Binary weights for the plain-C++ runner (engine/src/swipe_net.cpp). Layout documented there."""
    import struct

    cfg = net.cfg
    sd = {k: v.detach().cpu().numpy().astype(np.float32) for k, v in net.state_dict().items()}
    out = bytearray()
    out += b"SKNN"
    out += struct.pack("<IIIII", 1, 0 if dtype == "f32" else 1, T_STEPS, F_IN, KEY_FEATS)
    out += struct.pack("<IIII", len(cfg["conv"]), cfg["kernel"], cfg["hidden"], cfg["layers"])
    out += struct.pack("<I", cfg["pointer"])
    for c in cfg["conv"]:
        out += struct.pack("<I", c)

    def put(a):
        a = np.ascontiguousarray(a, dtype=np.float32)
        out.extend(a.astype("<f4").tobytes() if dtype == "f32" else a.astype("<f2").tobytes())

    for i in range(len(cfg["conv"])):
        put(sd[f"convs.{i}.weight"])      # (cout, cin, k)
        put(sd[f"convs.{i}.bias"])
    for layer in range(cfg["layers"]):
        for suffix in ("", "_reverse"):
            put(sd[f"gru.weight_ih_l{layer}{suffix}"])   # (3H, in), gate order r,z,n
            put(sd[f"gru.weight_hh_l{layer}{suffix}"])   # (3H, H)
            put(sd[f"gru.bias_ih_l{layer}{suffix}"])
            put(sd[f"gru.bias_hh_l{layer}{suffix}"])
    put(sd["A.weight"]); put(sd["A.bias"])      # (P, 2H), (P)
    put(sd["B.weight"]); put(sd["B.bias"])      # (P, 4), (P)
    put(sd["w.weight"]); put(sd["w.bias"])      # (1, P), (1)
    put(sd["blank.weight"]); put(sd["blank.bias"])   # (1, 2H), (1)
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def read_jsonl(path, limit=None):
    out = []
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f):
            if limit and i >= limit:
                break
            out.append(json.loads(line))
    return out
