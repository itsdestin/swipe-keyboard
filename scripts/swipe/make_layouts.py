# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Writes approximate letter-area layouts for other languages next to qwerty.tsv (same format as keys.tsv).

These are stand-ins for tests and synthetic training until the real per-language key rectangles are taken from the
keyboard itself: three rows, row 1 and 2 span the width, row 3 is inset. Run: python -I make_layouts.py
"""
import os

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "layouts")
YS = [1 / 6, 0.5, 5 / 6]


def write(name, rows, offsets):
    lines = ["letter\tcx\tcy\thalf_w\thalf_h"]
    for r, (keys, off) in enumerate(zip(rows, offsets)):
        n = max(len(x) for x in rows)
        w = 1.0 / n   # key width: the longest row fills the area
        for i, k in enumerate(keys):
            lines.append(f"{k}\t{off + (i + 0.5) * w:.4f}\t{YS[r]:.4f}\t{w / 2:.4f}\t{1 / 6:.4f}")
    open(os.path.join(HERE, name), "w", encoding="utf-8").write("\n".join(lines) + "\n")


write("azerty.tsv", ["azertyuiop", "qsdfghjklm", "wxcvbn"], [0, 0, 0.15])
write("qwertz.tsv", ["qwertzuiop", "asdfghjkl", "yxcvbnm"], [0, 0.05, 0.15])
write("qwerty-es.tsv", ["qwertyuiop", "asdfghjklñ", "zxcvbnm"], [0, 0, 0.15])
