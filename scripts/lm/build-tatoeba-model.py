#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Build one deterministic Tatoeba sentence n-gram pack from pinned local inputs.

Usage: build-tatoeba-model.py LANGUAGE EXPORT.bz2 DETAILED.bz2 DICTIONARY OUTDIR NGRAM-BUILDER
Only sentence ids with id % 10 != 0 train the model; the held-out tenth is reported
for corpus coverage. No sentence contents are emitted to logs.
"""
import array
import bz2
from collections import Counter
import hashlib
import os
import re
import sys
import subprocess
import unicodedata
from pathlib import Path

# WHY immutable export pins: Tatoeba exports update weekly; a rebuild must not silently
# train from a different corpus snapshot than the model metadata records.
PINS = {
    "es": ("a11b96f9e2d20d24cd8bc72e54656e0ef5597fe0d5c59b9e91a5a0b14f4c1e14", "1c1a471125889fee14c4ad1cbb85edb45e5160d56a4c933136433c6b67b5f48d", "spa"),
    "fr": ("b49a8777f4be14ba765eb591b7f4136b684e9d5a88a3b028a0ebcf172e1f6326", "3f8a63682f727eecc28222fc24a3b38a13ed380f8b2395be27dc01ad1ed926ab", "fra"),
    "de": ("ccd063727cf3b6d8c74a0e4696c6f898ba9cd62e0b132af6b082bb53b33278e8", "224edaef9c1fd23587bcaaf77d8c79d8a33b32deba90062b9c8e026706ea4934", "deu"),
}
WORD = re.compile(r"[^\W\d_]+(?:['’][^\W\d_]+)*", re.UNICODE)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def alpha(s):
    return "".join(c for c in unicodedata.normalize("NFC", s).lower() if c.isalpha())


def main():
    lang, export, detailed, dictionary, outdir, builder = sys.argv[1:]
    expected, detailed_expected, iso = PINS[lang]
    if sha256(export) != expected or sha256(detailed) != detailed_expected:
        raise SystemExit("sentence export or author-attribution export hash differs from the verified snapshot")
    vocab = {}
    for line in open(dictionary, encoding="utf-8"):
        p = line.find("word=")
        if p < 0:
            continue
        fields = dict(item.split("=", 1) for item in line.strip().split(",") if "=" in item)
        word = alpha(fields.get("word", ""))
        if word:
            try: frequency=int(fields.get("f", "0"))
            except ValueError: frequency=0
            previous=vocab.get(word)
            if previous is None or frequency>previous[1]: vocab[word]=(fields.get("word",word),frequency)
    words = sorted(vocab)
    if len(words) + 3 >= 1 << 18:
        raise SystemExit("dictionary exceeds n-gram id limit")
    ids = {w: i + 3 for i, w in enumerate(words)}
    os.makedirs(outdir, exist_ok=True)
    vocab_path = os.path.join(outdir, "vocab.tsv")
    train = array.array("I")
    case_forms = Counter()
    train_ids = set()
    unigram_counts = Counter()
    bigram_counts = Counter()
    heldout_sentences = heldout_tokens = train_sentences = 0
    with bz2.open(export, "rt", encoding="utf-8", errors="strict") as src:
        for line in src:
            fields = line.rstrip("\n").split("\t", 3)
            if len(fields) != 3 or fields[1] != iso:
                continue
            try:
                sid = int(fields[0])
            except ValueError:
                continue
            tokens = [alpha(t) for t in WORD.findall(fields[2])]
            tokens = [t for t in tokens if t]
            if len(tokens) < 2:
                continue
            if sid % 10 == 0:
                heldout_sentences += 1
                heldout_tokens += len(tokens)
                continue
            train_ids.add(sid)
            previous=1
            for surface in WORD.findall(fields[2]):
                token=alpha(surface)
                current=ids.get(token, 0)
                train.append(current)
                if current:
                    unigram_counts[current] += 1
                    bigram_counts[(previous,current)] += 1
                previous=current
                if current:
                    case_forms[(token, surface)] += 1
            train.append(2)
            train_sentences += 1
    if train_sentences < 1000:
        raise SystemExit(f"too few usable training sentences: {train_sentences}")
    unigram_best=max(unigram_counts, key=lambda word: (unigram_counts[word], -word))
    bigram_best={}
    for (previous,current), count in bigram_counts.items():
        if previous not in bigram_best or (count,current) > bigram_best[previous]:
            bigram_best[previous]=(count,current)
    held_uni_hits=held_bi_hits=held_eval=0
    with bz2.open(export, "rt", encoding="utf-8", errors="strict") as src:
        for line in src:
            fields=line.rstrip("\n").split("\t",3)
            if len(fields)!=3 or fields[1]!=iso: continue
            try: sid=int(fields[0])
            except ValueError: continue
            if sid%10!=0: continue
            seq=[ids.get(alpha(token),0) for token in WORD.findall(fields[2])]
            previous=1
            for current in seq:
                if current:
                    held_eval+=1
                    held_uni_hits += current==unigram_best
                    held_bi_hits += current==(bigram_best.get(previous,(0,unigram_best))[1])
                previous=current
    best_display = {}
    if lang == "de":
        for (canonical, surface), count in case_forms.items():
            previous=best_display.get(canonical)
            if previous is None or (count, surface) > previous:
                best_display[canonical]=(count, surface)
    with open(vocab_path + ".part", "w", encoding="utf-8", newline="\n") as f:
        for word in words:
            source_display, frequency=vocab[word]
            display=best_display.get(word, (0, source_display))[1] if lang == "de" else source_display
            f.write(f"{word}\t{frequency}\t{display}\n")
    os.replace(vocab_path + ".part", vocab_path)
    contributors = set()
    with bz2.open(detailed, "rt", encoding="utf-8", errors="strict") as src:
        for line in src:
            fields=line.rstrip("\n").split("\t")
            if len(fields) >= 4 and fields[1] == iso:
                try: sid=int(fields[0])
                except ValueError: continue
                if sid in train_ids and fields[3] not in ("", "\\N"):
                    contributors.add(fields[3])
    with open(os.path.join(outdir, "CONTRIBUTORS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("Tatoeba sentence contributors whose CC BY 2.0 FR sentences trained this model:\n")
        for author in sorted(contributors, key=str.casefold): f.write(author + "\n")
    stream = os.path.join(outdir, "train.u32")
    with open(stream + ".part", "wb") as f:
        train.tofile(f)
    os.replace(stream + ".part", stream)
    run = os.path.join(outdir, "counts.run")
    subprocess.run([builder, "count", run, stream], check=True)
    model = os.path.join(outdir, "context.ngram")
    # A two-count floor retains useful inflections on these deliberately small, topic-skewed corpora.
    rc = os.spawnv(os.P_WAIT, builder, [builder, "build", "--vocab", vocab_path, "--out", model,
                     "--order", "3", "--t2", "2", "--t3", "2", run])
    if rc:
        raise SystemExit(rc)
    print(f"language={lang} export_sha256={expected} vocab={len(words)} train_sentences={train_sentences} "
          f"heldout_sentences={heldout_sentences} heldout_tokens={heldout_tokens} "
          f"heldout_known={held_eval} unigram_top1={held_uni_hits}/{held_eval} "
          f"bigram_top1={held_bi_hits}/{held_eval} authors={len(contributors)} "
          f"model_bytes={os.path.getsize(model)} model_sha256={sha256(model)}")


if __name__ == "__main__":
    main()
