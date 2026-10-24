# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""make-vocab.py DICT OUT — the word list the n-gram model knows: every word of the swipe dictionary in the
letters-only form a swipe spells (lower case: "don't" -> "dont"), most frequent first, with the
dictionary's f= value (the 0..255 frequency score) next to it.

WHY restricted to the dictionary: swipe guesses can only ever be dictionary words, so n-grams about other
words would only cost space. Any other word in the corpus or in typed context becomes <unk> ("some word").
WHY the letters-only form: it is what NgramLM::alphaForm (engine/include/swipekb/ngram.hpp) looks up, so model
lookups need no extra mapping."""
import sys


def alpha(word):
    # Same rule as NgramLM::alphaForm: lower-case, keep letters only.
    return "".join(c for c in word.lower() if c.isalpha())


def main():
    src, out = sys.argv[1], sys.argv[2]
    best = {}
    with open(src, encoding="utf-8") as f:
        for line in f:
            p = line.find("word=")
            if p < 0:
                continue
            fields = dict(kv.split("=", 1) for kv in line.strip().split(",") if "=" in kv)
            a = alpha(fields.get("word", ""))
            try:
                freq = int(fields.get("f", "0"))
            except ValueError:
                continue
            if a and freq > best.get(a, -1):
                best[a] = freq
    words = sorted(best.items(), key=lambda kv: (-kv[1], kv[0]))
    # WHY the limit: the model packs three word numbers into one 64-bit key, 18 bits each.
    if len(words) + 3 >= (1 << 18):
        sys.exit("vocabulary too large for 18-bit word ids")
    with open(out + ".part", "w", encoding="utf-8") as f:
        for w, freq in words:
            f.write(f"{w}\t{freq}\n")
    import os
    os.replace(out + ".part", out)
    print(f"{len(words)} words -> {out}")


if __name__ == "__main__":
    main()
