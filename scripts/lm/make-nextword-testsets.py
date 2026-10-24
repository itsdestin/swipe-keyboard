#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Fixed next-word test sets for one language, for tools/swipekb-nextword-eval (card #170).

Three sets, 2000 sentences each by default, chosen with a fixed seed so every model is scored on the same text:
  tatoeba.txt  Tatoeba sentences with id % 10 == 0 -- the tenth the shipped packs never trained on
               (scripts/lm/build-tatoeba-model.py). Home ground for today's Tatoeba-built pack.
  everyday.txt Common Voice's community-written sentences (CC0), minus any that also appear in Tatoeba.
  web.txt      Sentences from FineWeb-2's official test split (ODC-BY), at most one per web page.
WHY three: no single source is neutral. Today's pack was trained on Tatoeba, a new model on web text; a model
is "better" only if it wins on the sets it was not built from. Training text must later be checked against
these files for shared 8-word runs (leakage) before any score is trusted.

  make-nextword-testsets.py LANG TATOEBA.tsv.bz2 COMMONVOICE.txt FINEWEB2-TEST.parquet OUTDIR [--count N]
Needs pyarrow for the parquet file; run with -I.
"""
import argparse
import bz2
import random
import re
import unicodedata
from pathlib import Path

SENTENCE_END = re.compile(r"(?<=[.!?…])\s+")


def norm(text):
    return " ".join(unicodedata.normalize("NFC", text).split())


def key(text):
    # WHY letters only: "¿Vienes?" and "vienes" are the same sentence for overlap purposes.
    return "".join(c for c in norm(text).casefold() if c.isalnum() or c == " ").strip()


def usable(sentence):
    words = sentence.split()
    # WHY 4..30 words: shorter lines are mostly headings/fragments, longer ones run-ons or lists.
    return 4 <= len(words) <= 30 and not re.search(r"https?://|www\.|@|\d{4,}|[|<>{}\[\]#=_]", sentence)


def tatoeba(path, lang3):
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 3 and parts[1] == lang3 and int(parts[0]) % 10 == 0:
                yield norm(parts[2])


def web(path):
    import pyarrow.parquet as pq
    table = pq.read_table(path, columns=["text"])
    for text in table.column("text").to_pylist():
        candidates = [norm(s) for p in text.split("\n") for s in SENTENCE_END.split(p)]
        candidates = [s for s in candidates if usable(s) and s[0].isupper() and s[-1] in ".!?"]
        if candidates:
            # WHY one per page: pages repeat boilerplate; this keeps the set spread across sites.
            yield candidates[len(candidates) // 2]


def pick(sentences, count, seed, exclude=()):
    seen, out = set(exclude), []
    for s in sentences:
        k = key(s)
        if s and usable(s) and k not in seen:
            seen.add(k)
            out.append(s)
    random.Random(seed).shuffle(out)
    return out[:count]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lang")
    ap.add_argument("tatoeba")
    ap.add_argument("commonvoice")
    ap.add_argument("fineweb2_test")
    ap.add_argument("outdir")
    ap.add_argument("--count", type=int, default=2000)
    a = ap.parse_args()
    lang3 = {"es": "spa", "fr": "fra", "de": "deu", "en": "eng"}[a.lang]
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    all_tatoeba = list(tatoeba(a.tatoeba, lang3))
    sets = {"tatoeba": pick(all_tatoeba, a.count, 1)}
    with open(a.commonvoice, encoding="utf-8") as f:
        sets["everyday"] = pick((norm(l) for l in f), a.count, 2, exclude={key(s) for s in all_tatoeba})
    sets["web"] = pick(web(a.fineweb2_test), a.count, 3)
    for name, lines in sets.items():
        (out / f"{name}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{name}: {len(lines)} sentences")


if __name__ == "__main__":
    main()
