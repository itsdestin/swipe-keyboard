#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Write a trained checkpoint as the keyboard's model file (LANG.nwp.bin, format "SWKBNWP1").

  export.py CKPT LM_VOCAB.TXT OUT.nwp.bin [--f32]

File layout, all little-endian, every array row-major (see engine/src/nwp_model.cpp, which reads it):
  magic "SWKBNWP1" | u32 version=1 | u32 dtype (0 = f32, 1 = int8 with one f32 scale per row) |
  u32 V | u32 D | u32 H | u32 words_bytes | f32 lambda | f32 oov_bonus |   (how much to trust the model
  against the n-gram, see engine/src/nwp_predictor.cpp; tuned per language on held-back sentences)
  words (V-3 letters-only words, '\\n' separated, token k+3), zero-padded to a multiple of 16 |
  emb[V][D] (+ scale[V]; every int8 array is zero-padded to a multiple of 16 bytes) | out_bias[V] f32 | W_ih[4H][D] (+ scale[4H]) | W_hh[4H][D] (+ scale[4H]) |
  bias[4H] f32 (= b_ih + b_hh) | W_proj[D][H] (+ scale[D]) | u64 FNV-1a checksum of everything before it.
Gate order is PyTorch's (input, forget, cell, output). Tokens: 0 <unk>, 1 <s>, 2 </s>.
WHY int8 per row: the vocabulary table is ~70% of the file; one byte per number keeps a 24k-word model near
4 MB (the Gboard size class) and per-row scales keep rare-word rows as accurate as common ones.
WHY a checksum: a half-copied or damaged download must be refused, not produce wrong suggestions.
"""
import argparse
import struct
import sys

import numpy as np

MAGIC = b"SWKBNWP1"


def fnv1a(data):
    # numpy-free streaming FNV-1a 64 would be slow in Python; use the standard folded form over 8-byte lanes?
    # No: the C++ side must agree, so keep the byte-wise definition and let Python take its time on a few MB.
    h = 0xcbf29ce484222325
    for b in memoryview(data):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def quant(w):
    scale = np.abs(w).max(axis=1) / 127.0
    scale[scale == 0] = 1.0
    q = np.clip(np.rint(w / scale[:, None]), -127, 127).astype(np.int8)
    return q, scale.astype("<f4")


def write_model(path, words, sd, int8=True, lam=0.5, beta=0.0):
    """sd: dict of numpy float32 arrays named emb, bias, w_ih, w_hh, b_ih, b_hh, w_hr."""
    V, D = sd["emb"].shape
    H = sd["w_ih"].shape[0] // 4
    assert len(words) == V - 3 and sd["w_hh"].shape == (4 * H, D) and sd["w_hr"].shape == (D, H)
    wb = ("\n".join(words)).encode("utf-8")
    wb += b"\0" * (-len(wb) % 16)
    out = [MAGIC, struct.pack("<6I2f", 1, 1 if int8 else 0, V, D, H, len(wb), lam, beta), wb]

    def matrix(w):
        w = np.ascontiguousarray(w, dtype=np.float32)
        if int8:
            q, s = quant(w)
            raw = q.tobytes()
            out.append(raw + b"\0" * (-len(raw) % 16)); out.append(s.tobytes())
        else:
            out.append(w.astype("<f4").tobytes())

    matrix(sd["emb"])
    out.append(sd["bias"].astype("<f4").tobytes())
    matrix(sd["w_ih"]); matrix(sd["w_hh"])
    out.append((sd["b_ih"] + sd["b_hh"]).astype("<f4").tobytes())
    matrix(sd["w_hr"])
    body = b"".join(out)
    with open(path + ".part", "wb") as f:
        f.write(body); f.write(struct.pack("<Q", fnv1a(body)))
    import os
    os.replace(path + ".part", path)
    return len(body) + 8


def from_checkpoint(ck):
    m = ck["model"]
    g = lambda k: m[k].float().cpu().numpy()
    return {"emb": g("emb.weight"), "bias": g("bias"), "w_ih": g("lstm.weight_ih_l0"), "w_hh": g("lstm.weight_hh_l0"),
            "b_ih": g("lstm.bias_ih_l0"), "b_hh": g("lstm.bias_hh_l0"), "w_hr": g("lstm.weight_hr_l0")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ckpt"); ap.add_argument("vocab"); ap.add_argument("out")
    ap.add_argument("--f32", action="store_true")
    ap.add_argument("--lambda", dest="lam", type=float, default=0.5)
    ap.add_argument("--beta", type=float, default=0.0)
    a = ap.parse_args()
    import torch
    ck = torch.load(a.ckpt, map_location="cpu")
    words = open(a.vocab, encoding="utf-8").read().split("\n")[:-1]
    print("bytes", write_model(a.out, words, from_checkpoint(ck), int8=not a.f32, lam=a.lam, beta=a.beta))


if __name__ == "__main__":
    sys.exit(main())
