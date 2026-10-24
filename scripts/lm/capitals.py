# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""capitals.py LANG DICTIONARY OUT.tsv --parquet FILE... [--max-words N] [--share S] [--min-count C]

Which words the keyboard should show with a capital letter (I, I'm, Monday, Paris, Madrid, iPhone), counted from the
same open web text as the word lists (FineWeb / FineWeb-2, ODC-By 1.0).

Output: one "word<TAB>Form<TAB>share" line per word whose capitalised form is used at least --share of the time when the word is
NOT at the start of a sentence, for words of DICTIONARY (a .combined word list) only.

WHY a share and not the most frequent form (the word lists' rule): web text capitalises many ordinary words inside
names ("Banco Santander", "Universidad de Chile", "Hall of Fame"), so "Banco" or "Hall" can be the single most common
form while a person typing "el banco" or "the hall" wants lower case. Only a word that is nearly always capitalised,
like a place, a month or "I", is shown capitalised; everything else stays lower case, as before.
WHY not sentence starts: there every word is capitalised, which says nothing about the word.

Run with -I; needs pyarrow."""
import argparse
import importlib.util
import os
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor

# WHY import, not copy: counted with exactly the tokens the word lists and n-gram packs see.
_spec = importlib.util.spec_from_file_location(
    "count_words", os.path.join(os.path.dirname(os.path.abspath(__file__)), "count-words.py"))
_count = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_count)


def _init(banned):
    _count.init_worker(banned)


def _group(job):
    # WHY a wrapper here: worker processes find functions by module name, and count-words.py (a hyphenated file loaded
    # by path) has no importable name; this module does.
    return _count.count_group(job)


# Ordinary words that are nearly always capitalised in web text only because they sit inside a name ("Estados Unidos",
# "États-Unis", "United Kingdom"); a person typing "estamos unidos" or "unis" wants lower case. The English dictionary
# says so itself (see dictionary_words); the Spanish and French ones keep one form per word, so they are listed here.
KEEP_LOWER = {
    "es": {"unidos", "unidas", "unido", "unida"},
    "fr": {"unis", "unies", "uni", "unie", "etats"},
}


def dictionary_words(path):
    """Words of the dictionary, and those whose most common form there is lower case."""
    words, best = set(), {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith(" word="):
                form = _count.surface(line[6:line.index(",")])
                freq = int(line.split(",f=", 1)[1].split(",", 1)[0]) if ",f=" in line else 0
                key = form.lower()
                words.add(key)
                lower, capped = best.get(key, (-1, -1))
                best[key] = (max(lower, freq), capped) if form == key else (lower, max(capped, freq))
    # WHY both must agree (the dictionary's own most common form AND --share of the web text): "united" (only lower
    # case in the English dictionary) or "hall" (133 lower vs 134 "Hall") stay lower case; "Monday", "I" do not.
    lower_first = {k for k, (lower, capped) in best.items() if lower >= capped}
    return words, lower_first


def capitals(inner, keys, share, min_count):
    """{word: (Form, share)} for the words whose capitalised inner forms reach `share` of their inner uses."""
    by_key = {}
    for form, n in inner.items():
        key = form.lower()
        if key in keys:
            by_key.setdefault(key, Counter())[form] += n
    out = {}
    for key, forms in by_key.items():
        total = sum(forms.values())
        if total < min_count:
            continue
        # WHY "not all lower case" rather than "first letter upper": keeps iPhone, eBay, YouTube as people write them.
        capped = Counter({f: n for f, n in forms.items() if f != key})
        if capped and sum(capped.values()) >= share * total:
            # WHY all-caps only for short words: "NASA", "UK" are written that way; a long all-caps form is shouting.
            best = max(capped, key=lambda f: (capped[f], f))
            if best.isupper() and len(best) > 4 and not key.isupper():
                continue
            out[key] = (best, sum(capped.values()) / total)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lang")
    ap.add_argument("dictionary")
    ap.add_argument("out")
    ap.add_argument("--parquet", nargs="+", required=True)
    ap.add_argument("--max-words", type=int, default=100_000_000)
    ap.add_argument("--share", type=float, default=0.9)
    ap.add_argument("--min-count", type=int, default=50)
    ap.add_argument("--workers", type=int, default=os.cpu_count())
    a = ap.parse_args()
    import pyarrow.parquet as pq
    keys, lower_first = dictionary_words(a.dictionary)
    keys -= lower_first | KEEP_LOWER.get(a.lang, set())
    jobs = [(p, g) for p in a.parquet for g in range(pq.ParquetFile(p).num_row_groups)]
    inner, words = Counter(), 0
    with ProcessPoolExecutor(a.workers, initializer=_init, initargs=(frozenset(),)) as pool:
        # WHY batches in file order: stop near --max-words without counting the whole shard, and count the same
        # leading slice every run, so the list is reproducible.
        for start in range(0, len(jobs), a.workers):
            for _occ, _pages, _all, spell_inner, w, _s in pool.map(_group, jobs[start:start + a.workers]):
                inner.update(spell_inner)
                words += w
            if words >= a.max_words:
                break
    found = capitals(inner, keys, a.share, a.min_count)
    with open(a.out + ".part", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# capitals language={a.lang} words={words} share={a.share} min_count={a.min_count}\n")
        for key in sorted(found):
            form, used = found[key]
            f.write(f"{key}\t{form}\t{used:.3f}\n")
    os.replace(a.out + ".part", a.out)
    print(f"{a.lang}: {len(found)} capitalised words from {words} words of text", file=sys.stderr)


if __name__ == "__main__":
    main()
