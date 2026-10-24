#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Turn open text into the token files the next-word neural model (NWP LM) trains on (card #170).

  prepare.py LANG WORDCOUNTS VOCAB.TSV OUTDIR --parquet FILE... --exclude TESTFILE...
             [--size 24000] [--max-words 400000000] [--casual-weight 2] [--tatoeba BZ2 --tatoeba-weight 2]

Writes OUTDIR/lm_vocab.txt (one letters-only word per line; line k is token k+3) and OUTDIR/train.u16
(little-endian 16-bit tokens; 0 = <unk>, 1 = <s> (never stored, the trainer adds it), 2 = </s> ends a sentence).
WHY the same rules as the n-gram packs (scripts/lm/build-fineweb2-model.py is imported, not copied): the LM must
see exactly the text the 3-gram saw - same tokens, same casual up-weighting, same test-set exclusion (any
sentence sharing an 8-word run with a test file is dropped), same Tatoeba training tenths (id % 10 != 0).
WHY the vocabulary is the most frequent words that the keyboard's own word list also has: a suggestion must be
a word the keyboard can show; everything else stays with the n-gram (hybrid, see engine/src/nwp_predictor.cpp).
Run with -I; needs pyarrow and numpy.
"""
import argparse
import importlib.util
import os
import sys
import multiprocessing
import sys as _sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
# WHY two places: in the repo the helper lives one folder up; on the training machine the two files sit together.
FW2 = next(p for p in (os.path.join(HERE, "build-fineweb2-model.py"), os.path.join(HERE, "..", "build-fineweb2-model.py")) if os.path.exists(p))
spec = importlib.util.spec_from_file_location("fw2", FW2)
fw2 = importlib.util.module_from_spec(spec)
_sys.modules["fw2"] = fw2   # WHY: worker functions are sent to the pool by module name
spec.loader.exec_module(fw2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lang")
    ap.add_argument("wordcounts")
    ap.add_argument("vocab_tsv")
    ap.add_argument("outdir")
    ap.add_argument("--parquet", nargs="+", required=True)
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--size", type=int, default=24000)
    ap.add_argument("--max-words", type=int, default=400_000_000)
    ap.add_argument("--casual-weight", type=int, default=2)
    ap.add_argument("--tatoeba")
    ap.add_argument("--tatoeba-weight", type=int, default=2)
    ap.add_argument("--procs", type=int, default=12)
    a = ap.parse_args()

    keyboard_words = {fw2.alpha(l.split("\t")[0]) for l in open(a.vocab_tsv, encoding="utf-8")}
    words = []
    for line in open(a.wordcounts, encoding="utf-8"):
        if line.startswith("#"):
            continue
        w = fw2.alpha(line.split("\t")[0])
        if w and w in keyboard_words and len(words) < a.size:
            words.append(w)
    words = list(dict.fromkeys(words))
    ids = {w: i + 3 for i, w in enumerate(words)}
    banned = set()
    for path in a.exclude:
        for line in open(path, encoding="utf-8"):
            banned |= fw2.runs([t for t in (fw2.alpha(x) for x in fw2.WORD.findall(line)) if t])
    os.makedirs(a.outdir, exist_ok=True)
    with open(os.path.join(a.outdir, "lm_vocab.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(words) + "\n")

    import pyarrow.parquet as pq
    jobs = [(p, g) for p in a.parquet for g in range(pq.ParquetFile(p).num_row_groups)]
    total = sentences = casual = dropped = covered = 0
    out = open(os.path.join(a.outdir, "train.u16.part"), "wb")
    # WHY fork: newer Pythons default to forkserver, whose workers cannot import this file-path module.
    with multiprocessing.get_context("fork").Pool(a.procs, fw2.init_worker, (ids, banned, a.casual_weight)) as pool:
        for data, w, s, c, d in pool.imap(fw2.encode_group, jobs):
            arr = np.frombuffer(data, dtype=np.uint32)
            arr.astype("<u2").tofile(out)
            covered += int((arr > 2).sum())
            total, sentences, casual, dropped = total + w, sentences + s, casual + c, dropped + d
            if total >= a.max_words:
                pool.terminate()
                break
    if a.tatoeba:
        extra = fw2.tatoeba_stream(a.tatoeba, {"es": "spa", "fr": "fra", "de": "deu", "en": "eng"}[a.lang], ids, banned)
        arr = np.frombuffer(extra, dtype=np.uint32).astype("<u2")
        for _ in range(a.tatoeba_weight):
            arr.tofile(out)
        print(f"tatoeba tokens={len(arr)} x{a.tatoeba_weight}")
    out.close()
    os.replace(os.path.join(a.outdir, "train.u16.part"), os.path.join(a.outdir, "train.u16"))
    print(f"language={a.lang} vocab={len(words)} web_words={total} sentences={sentences} casual_pages={casual} "
          f"test_overlap_dropped={dropped} known_tokens={covered}")


if __name__ == "__main__":
    sys.exit(main())
