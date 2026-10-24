#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Build one language's next-word n-gram pack from FineWeb-2 web text (card #170).

  build-fineweb2-model.py LANG DICTIONARY OUTDIR NGRAM-BUILDER --parquet FILE... [--max-words N]
      [--casual-weight W] [--exclude TESTFILE...] [--t2 N] [--t3 N] [--stats-only]
      [--tatoeba EXPORT.bz2 --tatoeba-weight W] [--keep-apostrophes]

Same token rules, word list and file format as build-tatoeba-model.py, so the keyboard loads the result
unchanged: words are NFC, lower-cased, letters with inner apostrophes; ids come from the language's swipe
dictionary; anything else becomes <unk>.
WHY --keep-apostrophes: by default words are letters only, so "were"/"we're" and "its"/"it's" are one word and the
keyboard cannot tell them apart from the sentence. With it, a word keeps its inner apostrophes (curly ones written
straight), each dictionary spelling is its own word, and text written without the apostrophe ("dont", "im") counts for
the dictionary's spelling when the dictionary has no such word itself. The keyboard notices the apostrophes in the word
list (engine/src/ngram.cpp) and matches words the same way; packs without them keep working as before.
WHY FineWeb-2: billions of openly licensed (ODC-BY) words per language, against ~0.5M sentences in Tatoeba.
WHY a casual weight: web text skews formal; forum and Q&A pages (picked by URL) are counted W times so
everyday phrasing carries more weight. WHY --exclude: any sentence sharing an 8-word run with a test set is
dropped, so test scores are not inflated by text the model has memorised.
WHY optional Tatoeba: its short everyday sentences cover conversational phrasing web text lacks; only its
training tenths (id % 10 != 0) are used, weighted W times. Its CC BY 2.0 FR credits then apply to the pack.
Run with -I; needs pyarrow.
"""
import argparse
import array
import bz2
import os
import re
import subprocess
import unicodedata
from collections import Counter
from multiprocessing import Pool

WORD = re.compile(r"[^\W\d_]+(?:['’][^\W\d_]+)*", re.UNICODE)
SENTENCE = re.compile(r"(?<=[.!?…])\s+|\n+")
# WHY URL words, not a style model: cheap, explainable and good enough to pick discussion pages.
CASUAL_URL = re.compile(r"foro|forum|comunidad|respuesta|pregunta|comment|comentario|hilo|thread|topic|"
                        r"discus|community|answers|question|chat", re.I)
RUN = 8


def alpha(s):
    return "".join(c for c in unicodedata.normalize("NFC", s).lower() if c.isalpha())


def apostrophe_key(s):
    """Lower case, letters plus inner apostrophes (curly written straight): NgramLM::form() for such packs."""
    s = unicodedata.normalize("NFC", s).lower().replace("\u2019", "'")
    return re.sub("'+", "'", "".join(c for c in s if c.isalpha() or c == "'")).strip("'")


KEY = alpha


def runs(tokens):
    return {" ".join(tokens[i:i + RUN]) for i in range(len(tokens) - RUN + 1)}


def load_vocab(dictionary):
    vocab = {}
    for line in open(dictionary, encoding="utf-8"):
        fields = dict(item.split("=", 1) for item in line.strip().split(",") if "=" in item)
        word = KEY(fields.get("word", ""))
        if word:
            try:
                frequency = int(fields.get("f", "0"))
            except ValueError:
                frequency = 0
            if word not in vocab or frequency > vocab[word][1]:
                vocab[word] = (fields.get("word", word), frequency)
            if "nonword" in fields.get("flags", "") or fields.get("not_a_word") == "true":
                NONWORDS.add(word)
    return vocab


NONWORDS = set()   # dictionary entries flagged "not a word" ("whats"): kept for autocorrect only


def init_worker(ids, banned, casual_weight, key=alpha):
    global IDS, BANNED, WEIGHT, KEY
    IDS, BANNED, WEIGHT, KEY = ids, banned, casual_weight, key


def word_id(token, ids):
    """A text token's word number; with apostrophes kept, "dont" falls back to the dictionary's "don't"."""
    found = ids.get(token)
    if found is None and KEY is not alpha:
        found = ids.get(("\0", alpha(token)))   # the alias entries main() adds
    return found or 0


def encode_group(job):
    """One parquet row group -> (u32 id stream bytes, words, sentences, casual pages, dropped sentences)."""
    import pyarrow.parquet as pq
    path, group = job
    table = pq.ParquetFile(path).read_row_group(group, columns=["text", "url"])
    out = array.array("I")
    words = sentences = casual = dropped = 0
    for text, url in zip(table.column("text").to_pylist(), table.column("url").to_pylist()):
        repeat = WEIGHT if url and CASUAL_URL.search(url) else 1
        casual += repeat > 1
        page = array.array("I")
        for sentence in SENTENCE.split(text):
            tokens = [KEY(t) for t in WORD.findall(sentence)]
            tokens = [t for t in tokens if t]
            if len(tokens) < 2:
                continue
            # WHY letters-only for the overlap check: the test-set exclusion must not depend on the token rule.
            if BANNED and len(tokens) >= RUN and not runs([alpha(t) for t in tokens]).isdisjoint(BANNED):
                dropped += 1
                continue
            page.extend(word_id(t, IDS) for t in tokens)
            page.append(2)
            words += len(tokens)
            sentences += 1
        for _ in range(repeat):
            out.extend(page)
    return out.tobytes(), words, sentences, casual, dropped


def tatoeba_stream(path, lang3, ids, banned):
    """Tatoeba training sentences (id % 10 != 0) as an id stream, with build-tatoeba-model.py's token rules."""
    out = array.array("I")
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t", 3)
            if len(fields) != 3 or fields[1] != lang3 or not fields[0].isdigit() or int(fields[0]) % 10 == 0:
                continue
            tokens = [t for t in (KEY(x) for x in WORD.findall(fields[2])) if t]
            if len(tokens) < 2 or (banned and len(tokens) >= RUN and not runs([alpha(t) for t in tokens]).isdisjoint(banned)):
                continue
            out.extend(word_id(t, ids) for t in tokens)
            out.append(2)
    return out


def token_ids(words, vocab, keep_apostrophes):
    """Word number of every dictionary word (sorted list `words`), plus the apostrophe aliases used by word_id()."""
    ids = {w: i + 3 for i, w in enumerate(words)}
    if keep_apostrophes:
        # WHY aliases: web text often drops the apostrophe ("dont", "im"); when the dictionary has no such word, those
        # tokens count for its most frequent spelling with the same letters, as they did in letters-only packs.
        # A word the dictionary flags "not a word" ("whats") counts for the real spelling ("what's") the same way.
        best = {}
        for w in words:
            letters = alpha(w)
            if w == letters or w in NONWORDS:
                continue
            if (letters not in ids or letters in NONWORDS) and (letters not in best or vocab[w][1] > vocab[best[letters]][1]):
                best[letters] = w
        ids.update({("\0", letters): ids[w] for letters, w in best.items()})
        for letters, w in best.items():
            if letters in ids:
                ids[letters] = ids[w]
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lang")
    ap.add_argument("dictionary")
    ap.add_argument("outdir")
    ap.add_argument("builder")
    ap.add_argument("--parquet", nargs="+", required=True)
    ap.add_argument("--max-words", type=int, default=300_000_000)
    ap.add_argument("--casual-weight", type=int, default=2)
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--t2", type=int, default=2)
    ap.add_argument("--t3", type=int, default=2)
    ap.add_argument("--stats-only", action="store_true")
    ap.add_argument("--tatoeba")
    ap.add_argument("--tatoeba-weight", type=int, default=1)
    ap.add_argument("--keep-apostrophes", action="store_true")
    a = ap.parse_args()
    global KEY
    KEY = apostrophe_key if a.keep_apostrophes else alpha

    import pyarrow.parquet as pq
    vocab = load_vocab(a.dictionary)
    words = sorted(vocab)
    if len(words) + 3 >= 1 << 18:
        raise SystemExit("dictionary exceeds n-gram id limit")
    ids = token_ids(words, vocab, a.keep_apostrophes)
    banned = set()
    for path in a.exclude:
        for line in open(path, encoding="utf-8"):
            banned |= runs([t for t in (alpha(x) for x in WORD.findall(line)) if t])
    os.makedirs(a.outdir, exist_ok=True)
    vocab_path = os.path.join(a.outdir, "vocab.tsv")
    with open(vocab_path + ".part", "w", encoding="utf-8", newline="\n") as f:
        for word in words:
            display, frequency = vocab[word]
            f.write(f"{word}\t{frequency}\t{display}\n")
    os.replace(vocab_path + ".part", vocab_path)

    jobs = [(p, g) for p in a.parquet for g in range(pq.ParquetFile(p).num_row_groups)]
    stream = os.path.join(a.outdir, "train.u32")
    total = sentences = casual = dropped = 0
    with open(stream + ".part", "wb") as f, Pool(os.cpu_count(), init_worker, (ids, banned, a.casual_weight, KEY)) as pool:
        for data, w, s, c, d in pool.imap(encode_group, jobs):
            f.write(data)
            total, sentences, casual, dropped = total + w, sentences + s, casual + c, dropped + d
            if total >= a.max_words:
                pool.terminate()
                break
        if a.tatoeba:
            extra = tatoeba_stream(a.tatoeba, {"es": "spa", "fr": "fra", "de": "deu", "en": "eng"}[a.lang], ids, banned)
            for _ in range(a.tatoeba_weight):
                extra.tofile(f)
    os.replace(stream + ".part", stream)
    run = os.path.join(a.outdir, "counts.run")
    subprocess.run([a.builder, "count", run, stream], check=True)
    if a.stats_only:
        subprocess.run([a.builder, "stats", run], check=True)
        return
    model = os.path.join(a.outdir, "context.ngram")
    subprocess.run([a.builder, "build", "--vocab", vocab_path, "--out", model, "--order", "3",
                    "--t2", str(a.t2), "--t3", str(a.t3), run], check=True)
    print(f"apostrophes={int(a.keep_apostrophes)} language={a.lang} words={total} sentences={sentences} casual_pages={casual} "
          f"casual_weight={a.casual_weight} tatoeba_weight={a.tatoeba_weight if a.tatoeba else 0} test_overlap_dropped={dropped} vocab={len(words)} "
          f"t2={a.t2} t3={a.t3} model_bytes={os.path.getsize(model)}")


if __name__ == "__main__":
    main()
