#!/usr/bin/env python3
"""Count surface words in the openly licensed training text, to build word lists (card #170, Phase 1).

  count-words.py LANG OUT.tsv --parquet FILE... [--max-words N] [--exclude TESTFILE...]
      [--tatoeba EXPORT.bz2] [--keep N] [--min-pages P]

For every word (lower-cased) it writes: how often it occurs, on how many distinct pages, and the way it is
normally spelled (capitals kept: "Berlin", "Haus", but "und").
WHY this exists: the es/fr/de/en word lists used to come from wordfreq (CC BY-SA 4.0, share-alike). Counting the
text we already train on (FineWeb / FineWeb-2, ODC-By 1.0; Tatoeba, CC BY 2.0 FR) leaves only attribution terms.

Same text as the n-gram packs: the first --max-words words of the given parquet files (in order, like
build-fineweb2-model.py, whose token rules are imported so the two can never drift), plus Tatoeba's training
tenths (id % 10 != 0), and sentences sharing an 8-word run with a --exclude test file are dropped, so test text
never feeds the counts. Each Tatoeba sentence counts as one page.

Output (tab separated, most frequent first; the first line is a comment with the totals):
  # total_words=N language=xx
  word<TAB>count<TAB>pages<TAB>spelling
`word` is NFC, lower case, typographic apostrophes turned into '; `spelling` is the most frequent form that does
NOT start a sentence (sentence-initial capitals say nothing about a word's real casing); if a word never occurs
elsewhere, the most frequent form overall. Only the --keep most frequent words with >= --min-pages pages are
written. WHY a cut: the file is the pinned input of the dictionary build; the rest is single-page noise.
Run with -I; needs pyarrow for parquet input.
"""
import argparse
import bz2
import importlib.util
import os
import sys
import unicodedata
from collections import Counter, deque
from concurrent.futures import ProcessPoolExecutor

# WHY import, not copy: the dictionary must be counted with exactly the tokens the n-gram builder sees.
_spec = importlib.util.spec_from_file_location(
    "build_fineweb2_model", os.path.join(os.path.dirname(os.path.abspath(__file__)), "build-fineweb2-model.py"))
_model = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_model)
WORD, SENTENCE, RUN, alpha, runs = _model.WORD, _model.SENTENCE, _model.RUN, _model.alpha, _model.runs


def surface(token):
    """NFC form with typographic apostrophes folded to '; case is kept."""
    return unicodedata.normalize("NFC", token).replace("’", "'")


GLUE = frozenset("0123456789_@/\\#")


def attached(sentence, start, end):
    """True when the word is glued to digits or address characters ("5th", "mp3", "a@b", "example.com", "u.s")."""
    before = sentence[start - 1] if start else ""
    after = sentence[end] if end < len(sentence) else ""
    if before in GLUE or after in GLUE or before.isdigit() or after.isdigit():
        return True
    # A dot between two letters ("example.com", "e.g"): sentence ends have a space after the dot, so are not caught.
    return (before == "." and start > 1 and sentence[start - 2].isalpha()) or \
           (after == "." and end + 1 < len(sentence) and sentence[end + 1].isalpha())


def count_page(text, banned, occ, pages, spell_all, spell_inner):
    """Adds one page (or one Tatoeba sentence) to the counters. Returns (words, kept sentences)."""
    seen = set()
    words = sentences = 0
    for sentence in SENTENCE.split(text):
        tokens, usable = [], []
        for m in WORD.finditer(sentence):
            # WHY alpha() as the emptiness test: build-fineweb2-model.py drops tokens that lower-case to nothing.
            if alpha(m.group()):
                tokens.append(m.group())
                usable.append(not attached(sentence, m.start(), m.end()))
        if len(tokens) < 2:
            continue
        if banned and len(tokens) >= RUN and not runs([alpha(t) for t in tokens]).isdisjoint(banned):
            continue
        # WHY every token counts toward the total but only free-standing ones toward words: the total must match the
        # text the n-gram builder sees (so rates are per word of that text), while "5th", "mp3" and "example.com"
        # would otherwise add "th", "mp" and "example" to the word list.
        words += len(tokens)
        sentences += 1
        for position, (token, free) in enumerate(zip(tokens, usable)):
            if not free:
                continue
            token = surface(token)
            key = token.lower()
            occ[key] += 1
            seen.add(key)
            spell_all[token] += 1
            if position:
                spell_inner[token] += 1
    for key in seen:
        pages[key] += 1
    return words, sentences


def init_worker(banned):
    global BANNED
    BANNED = banned


def count_group(job):
    """One parquet row group -> (occurrences, pages, spellings, non-initial spellings, words, sentences)."""
    import pyarrow.parquet as pq
    path, group = job
    table = pq.ParquetFile(path).read_row_group(group, columns=["text"])
    occ, pages, spell_all, spell_inner = Counter(), Counter(), Counter(), Counter()
    words = sentences = 0
    for text in table.column("text").to_pylist():
        w, s = count_page(text, BANNED, occ, pages, spell_all, spell_inner)
        words, sentences = words + w, sentences + s
    return occ, pages, spell_all, spell_inner, words, sentences


def tatoeba_counts(path, lang3, banned, occ, pages, spell_all, spell_inner):
    """Tatoeba training sentences (id % 10 != 0), each one a page, same selection as build-fineweb2-model.py."""
    words = 0
    with bz2.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t", 3)
            if len(fields) != 3 or fields[1] != lang3 or not fields[0].isdigit() or int(fields[0]) % 10 == 0:
                continue
            words += count_page(fields[2], banned, occ, pages, spell_all, spell_inner)[0]
    return words


def choose_spelling(forms_inner, forms_all):
    """Most frequent non-sentence-initial form; ties and never-inner words fall back to the overall counts."""
    pool = forms_inner or forms_all
    return max(pool, key=lambda form: (pool[form], forms_all.get(form, 0), form))


def write_counts(out, occ, pages, spell_all, spell_inner, total_words, keep, min_pages, lang):
    by_key_all, by_key_inner = {}, {}
    ranked = [k for k, _ in occ.most_common() if pages[k] >= min_pages][:keep]
    wanted = set(ranked)
    for form, n in spell_all.items():
        key = form.lower()
        if key in wanted:
            by_key_all.setdefault(key, {})[form] = n
    for form, n in spell_inner.items():
        key = form.lower()
        if key in wanted:
            by_key_inner.setdefault(key, {})[form] = n
    with open(out + ".part", "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# total_words={total_words} language={lang}\n")
        for key in ranked:
            shown = choose_spelling(by_key_inner.get(key, {}), by_key_all[key])
            f.write(f"{key}\t{occ[key]}\t{pages[key]}\t{shown}\n")
    os.replace(out + ".part", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lang")
    ap.add_argument("out")
    ap.add_argument("--parquet", nargs="*", default=[])
    ap.add_argument("--max-words", type=int, default=300_000_000)
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--tatoeba")
    ap.add_argument("--keep", type=int, default=400_000)
    ap.add_argument("--min-pages", type=int, default=3)
    a = ap.parse_args()

    banned = set()
    for path in a.exclude:
        for line in open(path, encoding="utf-8"):
            banned |= runs([t for t in (alpha(x) for x in WORD.findall(line)) if t])
    occ, pages, spell_all, spell_inner = Counter(), Counter(), Counter(), Counter()
    total = 0
    if a.parquet:
        import pyarrow.parquet as pq
        jobs = [(p, g) for p in a.parquet for g in range(pq.ParquetFile(p).num_row_groups)]
        # WHY ProcessPoolExecutor, not Pool: if a worker dies (killed for memory, say) Pool.imap waits forever for
        # its lost row group, while this raises BrokenProcessPool at once and the run can be repeated.
        workers = os.cpu_count()
        with ProcessPoolExecutor(workers, initializer=init_worker, initargs=(banned,)) as pool:
            pending, queue = deque(), iter(jobs)
            done = 0
            while True:
                # Keep a few row groups ahead per worker, but consume strictly in order: the slice must be
                # the same one build-fineweb2-model.py counts (it stops at the first row group past --max-words).
                while len(pending) < 2 * workers:
                    job = next(queue, None)
                    if job is None:
                        break
                    pending.append(pool.submit(count_group, job))
                if not pending:
                    break
                o, p, sa, si, w, s = pending.popleft().result()
                occ.update(o)
                pages.update(p)
                spell_all.update(sa)
                spell_inner.update(si)
                total += w
                done += 1
                if done % 200 == 0:
                    print(f"{a.lang}: {done}/{len(jobs)} row groups, {total} words", file=sys.stderr, flush=True)
                if total >= a.max_words:
                    for f in pending:
                        f.cancel()
                    break
    if a.tatoeba:
        lang3 = {"es": "spa", "fr": "fra", "de": "deu", "en": "eng"}[a.lang]
        total += tatoeba_counts(a.tatoeba, lang3, banned, occ, pages, spell_all, spell_inner)
    write_counts(a.out, occ, pages, spell_all, spell_inner, total, a.keep, a.min_pages, a.lang)
    print(f"language={a.lang} words={total} distinct={len(occ)} written<={a.keep} out={a.out}")


if __name__ == "__main__":
    main()
