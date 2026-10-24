#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Regenerate the tiny test model and PyTorch's answers for it (tests/data/nwp-tiny*.{nwp.bin,expected}).

  make-fixture.py OUTDIR      (needs torch; run from scripts/lm/nwp)

WHY: tst_nwpmodel proves the C++ runner computes what PyTorch computes - it feeds token sequences through the
runner and compares the log-probabilities with the numbers written here. Random weights are enough: every
step of the maths (gates, cell, projection, tied output, softmax) is exercised and any slip shows.
The int8 file's expected numbers come from the float model rebuilt from the file's own rounded weights, so the
comparison isolates the runner (the rounding itself is measured separately on real models).
"""
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export
from train import NWP

WORDS = ["the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "he", "was", "for", "on", "are", "as",
         "with", "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by", "word", "but"]
SEQS = [[1], [1, 5], [1, 7, 12, 3, 9], [1, 3, 3, 3, 3, 3, 3, 3], [0, 4, 2, 8], [1, 33 - 1, 20, 21, 22, 10, 11, 12, 13]]


def main():
    out = sys.argv[1]
    torch.manual_seed(7)
    V, D, H = len(WORDS) + 3, 8, 12
    model = NWP(V, D, H)
    with torch.no_grad():
        for p in model.parameters():
            p.copy_(torch.randn_like(p) * 0.6)
    sd = export.from_checkpoint({"model": model.state_dict()})
    for name, int8 in (("nwp-tiny", False), ("nwp-tiny8", True)):
        path = os.path.join(out, name + ".nwp.bin")
        export.write_model(path, WORDS, sd, int8=int8, lam=0.7, beta=-0.25)
        ref = NWP(V, D, H)
        if int8:
            # Rebuild the weights exactly as the runner dequantises them.
            q = {k: (lambda m: m[0].astype(np.float32) * m[1][:, None])(export.quant(sd[k])) for k in ("emb", "w_ih", "w_hh", "w_hr")}
            sd2 = dict(sd, **q)
        else:
            sd2 = sd
        t = lambda k: torch.tensor(sd2[k])
        ref.load_state_dict({"emb.weight": t("emb"), "bias": t("bias"), "lstm.weight_ih_l0": t("w_ih"),
                             "lstm.weight_hh_l0": t("w_hh"), "lstm.bias_ih_l0": t("b_ih"), "lstm.bias_hh_l0": t("b_hh"),
                             "lstm.weight_hr_l0": t("w_hr")})
        with open(os.path.join(out, name + ".expected"), "w") as f:
            f.write("# tokens read ; natural-log probability of every token as the next one (PyTorch)\n")
            for seq in SEQS:
                with torch.no_grad():
                    logits, _ = ref(torch.tensor([seq]))
                    lp = torch.log_softmax(logits[0, -1], -1)
                f.write(" ".join(map(str, seq)) + " ; " + " ".join(f"{x:.7e}" for x in lp.tolist()) + "\n")
        print(name, os.path.getsize(path), "bytes")


if __name__ == "__main__":
    main()
