# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Previous words for every swipe of an .npz made by prep_data.py (joint-model step 4; python -I prep_context.py ...).

  prep_context.py swipes.jsonl --layout L.tsv --npz swipes.npz      -> swipes.ctx.npy (N, CTX_CHARS) int8

The .jsonl must be the file the .npz was made from, with the same layout: the swipes prep_data.py dropped are dropped
here by the same rule, and every kept swipe's word is checked against the .npz (a mismatch stops). The previous words
come from the record's sentence and word position (FUTO collections); a record without them gets none (all padding),
as synthetic swipes do (train.py gives an .npz without a .ctx.npy no previous words).
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("--layout", required=True)
    ap.add_argument("--npz", required=True)
    a = ap.parse_args()
    lay = S.Layout(a.layout)
    words = np.load(a.npz)["words"]
    out, known = [], 0
    with open(a.jsonl, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            # WHY the same test as prep_data._one: the rows must line up with the .npz one for one.
            if not r["data"]:
                continue
            keys = lay.word_keys(r["word"])
            if not keys or len(keys) > 30:
                continue
            if len(out) >= len(words) or words[len(out)] != r["word"]:
                raise SystemExit(f"row {len(out)}: {r['word']!r} does not match the .npz")
            before = S.words_before(r)
            known += bool(before)
            out.append(S.context_ids(before))
    if len(out) != len(words):
        raise SystemExit(f"{len(out)} rows but the .npz has {len(words)}")
    path = a.npz[:-4] + ".ctx.npy"
    np.save(path, np.array(out, np.int8))
    print(f"{path}: {len(out)} swipes, {known} with words before ({100 * known / max(1, len(out)):.1f}%)")


if __name__ == "__main__":
    main()
