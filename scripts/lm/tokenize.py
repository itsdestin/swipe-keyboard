# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""tokenize.py VOCAB PARQUET OUTDIR [WORKERS] — turn one FineWeb parquet shard into word-number streams.

Output: OUTDIR/part-NNN.u32, little-endian uint32 word numbers; 2 (</s>) ends every sentence. Numbers:
0 = <unk> (a word not in the swipe dictionary), 1 = <s> (sentence start, implied), 2 = </s>, 3+ = the
words of VOCAB in file order.

WHY these splitting rules: the model must see text the way the keyboard hands context to it
(swipekb::contextWords in engine/src/context.cpp): a . ! or ? starts a new sentence (a line break too: a
paragraph or list item is not one sentence with the next), words are split on white space, punctuation is
trimmed off both ends, and a "word" left with no letters (a number, "$5") is still a word, just an unknown
one. Then the letters-only lower-case form is looked up (the form a swipe spells).

WHY pages from wiki sites are skipped: the scoreboard's sentences come from Wikipedia; Web copies of
Wikipedia pages in the corpus would leak test sentences into the model and inflate the score."""
import array
import os
import re
import sys
from multiprocessing import Pool
from urllib.parse import urlsplit

import pyarrow.parquet as pq

# Same characters as isPunct in engine/src/context.cpp (Unicode punctuation, not symbols).
PUNCT = "!\"#%&'()*,-./:;?@[\\]_{}" + "¡§«¶·»¿" + "".join(
    chr(c) for r in [(0x2010, 0x2027), (0x2030, 0x2043), (0x2045, 0x2051), (0x2053, 0x205E),
                     (0x3001, 0x3003), (0x3008, 0x3011), (0x3014, 0x301F)] for c in range(r[0], r[1] + 1)
) + "！？，．"
SENT = re.compile(r"[.!?\n\r  ]+")
NONLETTER = re.compile(r"[\W\d_]+")
UNK, BOS, EOS = 0, 1, 2

VOCAB = None


def load_vocab(path):
    with open(path, encoding="utf-8") as f:
        return {line.split("\t", 1)[0]: i + 3 for i, line in enumerate(f)}


def wiki(url):
    try:
        host = urlsplit(url).hostname or ""
    except ValueError:
        return False
    return "wiki" in host or "dbpedia" in host


def work(job):
    path, groups, out = job
    f = pq.ParquetFile(path)
    ids = array.array("I")
    get = VOCAB.get
    docs = skipped = 0
    for g in groups:
        t = f.read_row_group(g, columns=["text", "url"])
        for text, url in zip(t.column("text").to_pylist(), t.column("url").to_pylist()):
            if url and wiki(url):
                skipped += 1
                continue
            docs += 1
            for sent in SENT.split(text):
                n = 0
                for tok in sent.split():
                    a = NONLETTER.sub("", tok.lower())
                    if a:
                        ids.append(get(a, UNK))
                        n += 1
                    elif tok.strip(PUNCT):
                        ids.append(UNK)  # a number or symbol: a word, but not one the keyboard can swipe
                        n += 1
                if n:
                    ids.append(EOS)
    with open(out + ".part", "wb") as o:
        ids.tofile(o)
    os.replace(out + ".part", out)
    return docs, skipped, len(ids)


def init(vocab_path):
    global VOCAB
    VOCAB = load_vocab(vocab_path)


def main():
    vocab, parquet, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    workers = int(sys.argv[4]) if len(sys.argv) > 4 else max(1, (os.cpu_count() or 2) - 4)
    os.makedirs(outdir, exist_ok=True)
    n = pq.ParquetFile(parquet).metadata.num_row_groups
    per = max(1, n // (workers * 3))
    jobs = [(parquet, list(range(s, min(n, s + per))), os.path.join(outdir, f"part-{s // per:04d}.u32"))
            for s in range(0, n, per)]
    docs = skipped = toks = 0
    with Pool(workers, initializer=init, initargs=(vocab,)) as p:
        for d, s, t in p.imap_unordered(work, jobs):
            docs, skipped, toks = docs + d, skipped + s, toks + t
    print(f"{parquet}: {docs} documents, {skipped} wiki pages skipped, {toks} tokens (incl. sentence ends)")


if __name__ == "__main__":
    main()
