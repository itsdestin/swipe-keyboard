#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Word lists counted from openly licensed text (no wordfreq), card #170 Phase 1.

Input: the counts files written by scripts/lm/count-words.py (FineWeb / FineWeb-2, ODC-By 1.0, plus Tatoeba
training sentences, CC BY 2.0 FR). Output: AOSP-layout `.combined` dictionaries.

  corpus_dictionary.py --lang en --counts en.counts.tsv --base en_US_wordlist.combined --casing en_wordlist.combined
                       --out en_swipe.combined [--max 250000] [--typo-ratio 20] [--min-pages 3]
      English: every word of the AOSP base (Apache-2.0) exactly as before, then corpus words AOSP lacks.
  corpus_dictionary.py --lang es|fr|de --counts X.counts.tsv --out X.combined [--max 100000]
      Other languages: the most frequent words of the counts, after the junk filters below.
  Both take --foreign OTHER.counts.tsv ... (the other languages' counts, for the foreign-word filter).

Junk filters (all for the same reason: web text contains misspellings and page debris that must not become
words a swipe can type): at least --min-pages distinct pages; the language's letters only (language_pack's
alphabet rule); a rare word (under 1,000 uses per billion words; any word, in English's added tail) of 4+ letters is dropped
when a word one typo away is --typo-ratio times more common; a rare word 30 times more typical of another language's text is dropped (English
sentences inside Spanish pages); rare all-capital abbreviations; German pre-reform ß spellings ("daß" when "dass" is at
least as common); web-address debris ("https", "php", ...) is dropped. No wordfreq dependency.
"""
import argparse
import math
import os
import re
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import combined  # noqa: E402

# WHY a short fixed list: these are page markup that web text counts thousands of times and nobody swipes.
# ("pdf", "jpg", "com" are things people do type, so they stay.)
WEB_DEBRIS = frozenset("http https www html htm php aspx href nbsp utm xml json cgi css src img".split())
# WHY explicit one-letter words: a corpus counts every list label ("b", "c", "x") as a word.
SINGLE_LETTERS = {"es": "aeouy", "fr": "aàyô", "de": "", "en": ""}


def read_counts(path):
    """Returns (total_words, [(key, count, pages, spelling)]) in file order (most frequent first)."""
    rows, total = [], 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                for part in line[1:].split():
                    if part.startswith("total_words="):
                        total = int(part.split("=", 1)[1])
                continue
            key, count, pages, spelling = line.rstrip("\n").split("\t")
            rows.append((key, int(count), int(pages), spelling))
    if not total:
        raise ValueError(f"{path}: missing total_words header")
    return total, rows


def zipf(count, total):
    """Zipf value (log10 of uses per billion words) of a word counted `count` times in `total` words."""
    return math.log10(count / total * 1e9)


def f_language(z):
    """0..255 frequency for es/fr/de, the same scale language_pack.py always used (zipf 1 -> 1, 8 -> 255)."""
    return max(1, min(255, round((z - 1) * 255 / 7)))


def is_rare_acronym(spelling, count, total):
    return len(spelling) > 1 and spelling.isupper() and zipf(count, total) < ACRONYM_MAX_ZIPF


def old_german_spelling(key, count, table):
    """True for a pre-1996 spelling with sharp s ("daß", "muß", "ißt") whose reformed twin ("dass", "muss",
    "isst") is at least as common. WHY: both swipe on the same keys and the old forms are still all over web text;
    measured, "ißt" beat "ist" and "daß" beat "das" in the German swipe bench. "groß" stays: "gross" is rarer."""
    return "ß" in key and table.get(key.replace("ß", "ss"), 0) >= count


def edits1(word, letters):
    """Every string one typo away from `word` (one letter missing, extra or changed, or two letters swapped)."""
    for i in range(len(word) + 1):
        left, right = word[:i], word[i:]
        if right:
            yield left + right[1:]
            for c in letters:
                yield left + c + right[1:]
        if len(right) > 1:
            yield left + right[1] + right[0] + right[2:]
        for c in letters:
            yield left + c + right


def _bare(word):
    return "".join(c for c in unicodedata.normalize("NFD", word) if not unicodedata.combining(c))


def _accents(word):
    return sum(1 for c in unicodedata.normalize("NFD", word) if unicodedata.combining(c))


# WHY a cap on both filters: a misspelling or a stray English phrase is counted a few hundred times at most, while
# real words that merely sit near a far commoner one ("perro" next to "pero", "par" the Spanish word beside the
# French one) are used thousands of times. Without the cap the first filter deleted "perro", "pobre", "haces".
# Zipf is log10 of uses per billion words: 3.0 = 1,000 uses per billion, 4.0 = 10,000 per billion.
# WHY 3.0 and ratio 100 for es/fr/de (measured on French): at ratio 20 and zipf 3.5 the filter also removed real
# verb forms one letter from a commoner one ("allais", "aurai", "entends", "fondera"); at 100 what it removes is
# nearly all truncations, missing accents and English ("maximun", "quelqun", "aujour", "mdicament").
TYPO_MAX_ZIPF = 3.0
LANGUAGE_TYPO_RATIO = 100.0
# English adds only words AOSP lacks (the tail), so no cap. WHY 50, not wordfreq's 20: wordfreq blends sources and
# compresses frequency gaps; in raw web counts ratio 20 also removed real British spellings and verb forms ("learnt",
# "fibre", "tyres", "practise"). Real swipes (2,000, no n-gram): ratio 20 93.17% / 96.35%, 50 93.22% / 96.40%,
# 100 93.11% / 96.35%, all within noise of each other; 50 keeps most real words and drops "dont", "thats", "youre".
ENGLISH_TYPO_RATIO = 50.0
FOREIGN_MAX_ZIPF = 4.0
# WHY: all-capital tokens ("AUD", "MOZ", "IDR") are currency codes, abbreviations and shouting. Rare ones measured
# badly: in the German swipe bench "AUD" beat "aus". 3.4 (about 770 uses in 300M words) keeps "SSD", "VMC", "USA".
ACRONYM_MAX_ZIPF = 3.4
# WHY 4: three-letter words sit one typo from many commoner words ("vos"/"los") and are never worth guessing at.
TYPO_MIN_LENGTH = 4
# WHY 30 / 20 pages: English text inside Spanish pages makes "presentation" look Spanish; a real word that is
# 30x more typical of another language's text (German "gift" vs English "gift") is rare enough to give up.
FOREIGN_RATIO = 30.0
FOREIGN_MIN_PAGES = 20


def make_foreign_test(foreign_paths, total, ratio=FOREIGN_RATIO):
    """Returns is_foreign(word, count): True when another language's text uses `word` `ratio` times as often
    (per word of text) as this one does, backed by at least FOREIGN_MIN_PAGES pages there."""
    others = []
    for path in foreign_paths:
        other_total, rows = read_counts(path)
        others.append((other_total, {k: (c, p) for k, c, p, _ in rows}))

    def is_foreign(word, count):
        here = count / total
        if zipf(count, total) >= FOREIGN_MAX_ZIPF:
            return False
        for other_total, table in others:
            hit = table.get(word)
            if hit and hit[1] >= FOREIGN_MIN_PAGES and hit[0] / other_total >= ratio * here:
                return True
        return False
    return is_foreign


def make_typo_test(counts, alphabet, ratio, total, max_zipf=TYPO_MAX_ZIPF):
    """Returns looks_like_typo(word): a word one typo away is `ratio` times more common (0 = never).

    WHY accent pairs are treated separately: "sur"/"sûr", "ou"/"où", "fur"/"für" are different real words, and the
    rarer one often has the accent. Only a candidate with FEWER accents than its common neighbour ("educacion"
    next to "educación") is a typo; the same letters with more accents is a real word.
    """
    if not ratio:
        return lambda word: False
    letters = alphabet + "'"
    table = {key: count for key, count, _, _ in counts}

    def looks_like_typo(word):
        count = table.get(word, 0)
        if len(word) < TYPO_MIN_LENGTH or zipf(max(count, 1), total) >= max_zipf:
            return False
        need = count * ratio
        for other in edits1(word, letters):
            if other != word and table.get(other, 0) >= need:
                if _bare(other) == _bare(word) and _accents(word) >= _accents(other):
                    continue
                return True
        return False
    return looks_like_typo


def language_entries(counts_path, language, alphabet, clean, maximum, min_pages=3, typo_ratio=LANGUAGE_TYPO_RATIO, foreign=()):
    """es/fr/de: [(word, f)] most frequent first. `clean(word, language)` is language_pack.clean_word."""
    total, rows = read_counts(counts_path)
    typo = make_typo_test(rows, alphabet, typo_ratio, total)
    is_foreign = make_foreign_test(foreign, total)
    table = {key: count for key, count, _, _ in rows}
    out, seen = [], set()
    for key, count, pages, spelling in rows:
        if len(out) >= maximum:
            break
        if pages < min_pages or key in WEB_DEBRIS or key in seen or is_foreign(key, count):
            continue
        if len(key) == 1 and key not in SINGLE_LETTERS[language]:
            continue
        if is_rare_acronym(spelling, count, total) or old_german_spelling(key, count, table):
            continue
        # WHY the typographic apostrophe for French: the earlier French list used it for elisions (l’école).
        shown = spelling.replace("'", "’") if language == "fr" else spelling
        word = clean(shown, language)
        if word is None or typo(key):
            continue
        seen.add(key)
        out.append((word, f_language(zipf(count, total))))
    return out


# WHY this shape only: plain English words with at most one apostrophe ("don't", "o'clock"), as wordfreq_combined's
# PLAIN pattern; it drops numbers, web addresses and other scripts.
PLAIN_ENGLISH = re.compile(r"[a-z]+(?:'[a-z]+)?")


def english_entries(counts_path, base, casing_paths, maximum=250000, typo_ratio=ENGLISH_TYPO_RATIO, min_pages=3, foreign=()):
    """English: the AOSP base kept exactly (every word, frequency and flag, not-a-word lines dropped, apostrophe
    twins ordered), plus corpus words the base lacks, up to `maximum` words. Returns [(word, f, flags)]."""
    total, rows = read_counts(counts_path)
    best = {}
    for path in list(casing_paths) + [base]:
        for word, freq, _ in combined.read(path):
            k = word.lower()
            if k not in best or freq > best[k][1]:
                best[k] = (word, freq)
    casing = {k: w for k, (w, _) in best.items()}
    out, seen, exact = [], set(), set()
    bad = combined.not_words(base)
    for word, freq, flags in combined.read(base):
        seen.add(word.lower())
        if word in bad:
            continue
        if word not in exact:  # AOSP repeats a few words; one line each
            exact.add(word)
            out.append((word, freq, flags))
    typo = make_typo_test(rows, "abcdefghijklmnopqrstuvwxyz", typo_ratio, total, max_zipf=float("inf"))
    is_foreign = make_foreign_test(foreign, total)
    for key, count, pages, spelling in rows:
        if len(out) >= maximum:
            break
        if pages < min_pages or not PLAIN_ENGLISH.fullmatch(key) or key in seen or key in WEB_DEBRIS or is_foreign(key, count):
            continue
        # WHY at least 3 letters: AOSP already holds every real one- and two-letter English word, so what the corpus
        # adds there is fragments ("th", "pp", "ca") and list labels.
        if len(key) < 3 or is_rare_acronym(spelling, count, total):
            continue
        if typo(key):
            continue
        seen.add(key)
        # WHY AOSP's spelling first, then the corpus's most frequent non-sentence-initial form (names, acronyms).
        shown = casing.get(key) or spelling
        out.append((shown, combined.zipf_to_f(zipf(count, total)), ""))
    return combined.order_apostrophe_twins(out, combined.whitelist(base), exact)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lang", required=True, choices=["en", "es", "fr", "de"])
    ap.add_argument("--counts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int)
    ap.add_argument("--base")
    ap.add_argument("--casing", nargs="*", default=[])
    ap.add_argument("--typo-ratio", type=float, help="default 20 for English, 100 for the others")
    ap.add_argument("--min-pages", type=int, default=3)
    ap.add_argument("--foreign", nargs="*", default=[])
    a = ap.parse_args()
    if a.lang == "en":
        if not a.base:
            ap.error("--base is required for English")
        entries = english_entries(a.counts, a.base, a.casing, a.max or 250000, ENGLISH_TYPO_RATIO if a.typo_ratio is None else a.typo_ratio, a.min_pages, a.foreign)
        combined.write(a.out, entries, "English (AOSP + FineWeb counts)")
        print("%s: %d words" % (a.out, len(entries)))
        return
    import language_pack
    data = language_pack.dictionary_from_counts(a.counts, a.lang, a.max or 100000, a.min_pages, LANGUAGE_TYPO_RATIO if a.typo_ratio is None else a.typo_ratio, a.foreign)
    with open(a.out, "wb") as f:
        f.write(data)
    print("%s: %d bytes" % (a.out, len(data)))


if __name__ == "__main__":
    main()
