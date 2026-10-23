"""Builds a `.combined` dictionary from the `wordfreq` package's English "large" list (data CC-BY-SA 4.0).

Usage (inside a venv that has wordfreq; see build-candidates.sh):
  python -I wordfreq_combined.py --out OUT [--max N] [--base AOSP.combined] [--casing REF.combined ...]

  --base     merge mode: keep every word of this dictionary with its own frequency, then add wordfreq words
             it lacks (candidate d). Without --base the result is wordfreq only (candidate c).
  --casing   dictionaries whose spelling of capitals to copy ("I", "I'm", "London"). WHY: wordfreq stores
             every word in lower case, so on its own it would type "i" and "london".
  --typo-ratio R  drop an added word when a word one typo away (one letter missing, extra, changed or two
             letters swapped) is at least R times more common (default 20; 0 = keep all). WHY: wordfreq counts
             real web text, misspellings included ("memebers", "cemetary"); on the scoreboard they pushed the
             correct spelling out of first place.
  --max      keep at most N words, most frequent first (default 250000; the engine's tables are sized for
             about 400k entries, and every extra rare word is one more way to guess wrong).
"""
import argparse
import math
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import combined  # noqa: E402

from wordfreq import top_n_list, zipf_frequency  # noqa: E402

# WHY this shape only: plain English words with at most one apostrophe ("don't", "o'clock"). It drops numbers,
# web addresses, other scripts and wordfreq's tokenizer leftovers ("u.s", "e.g") that cannot be swiped.
PLAIN = re.compile(r"[a-z]+(?:'[a-z]+)?")


LETTERS = "abcdefghijklmnopqrstuvwxyz'"


def edits1(w):
    """Every string one typo away from w (Norvig's classic spelling-corrector set)."""
    splits = [(w[:i], w[i:]) for i in range(len(w) + 1)]
    for left, right in splits:
        if right:
            yield left + right[1:]
            for c in LETTERS:
                yield left + c + right[1:]
        if len(right) > 1:
            yield left + right[1] + right[0] + right[2:]
        for c in LETTERS:
            yield left + c + right


# WHY moved: the twin ordering is not wordfreq-specific; corpus_dictionary.py (no wordfreq) shares it.
from combined import TWIN_MARGIN, order_apostrophe_twins  # noqa: E402,F401


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--max", type=int, default=250000)
    ap.add_argument("--base")
    ap.add_argument("--casing", nargs="*", default=[])
    ap.add_argument("--typo-ratio", type=float, default=20.0)
    a = ap.parse_args()

    # Capitalization reference: lower-case key -> the reference's most frequent spelling of it. WHY most
    # frequent: AOSP lists both "will" (common) and "Will" (the name), and both "I" (common) and "i" (f=0,
    # marked not-a-word); the common spelling is the one a swipe should type.
    best = {}
    for path in a.casing + ([a.base] if a.base else []):
        for word, freq, _ in combined.read(path):
            k = word.lower()
            if k not in best or freq > best[k][1]:
                best[k] = (word, freq)
    casing = {k: w for k, (w, _) in best.items()}

    out, seen = [], set()
    if a.base:
        exact = set()
        # WHY drop AOSP's not-a-word lines ("im", "alot", "hed", f=0): the old build kept them as ordinary f=0
        # words. They stay in `seen`, so wordfreq cannot add them back either ("im" is very common web text).
        bad = combined.not_words(a.base)
        for word, freq, flags in combined.read(a.base):
            seen.add(word.lower())
            if word in bad:
                continue
            if word not in exact:  # WHY: AOSP repeats a few words; one line each keeps the file clean
                exact.add(word)
                out.append((word, freq, flags))

    ranked = [w for w in top_n_list("en", 10 ** 7, wordlist="large") if PLAIN.fullmatch(w)]
    zipf = {w: zipf_frequency(w, "en", wordlist="large") for w in ranked}
    # log10 of the ratio: Zipf is already log10, so "R times more common" = a Zipf gap of log10(R).
    gap = math.log10(a.typo_ratio) if a.typo_ratio > 0 else None

    def looks_like_typo(w):
        z = zipf[w] + gap
        for v in edits1(w):
            if zipf.get(v, 0.0) >= z:
                return True
        return False

    for w in ranked:
        if len(out) >= a.max:
            break
        if not PLAIN.fullmatch(w) or w in seen:
            continue
        # WHY: single letters the reference does not use as words (it has "a" and "I") are almost always swipe noise.
        if len(w) == 1 and best.get(w, ("", 0))[1] == 0:
            continue
        if gap is not None and looks_like_typo(w):
            continue
        seen.add(w)
        spelled = casing.get(w, w)
        out.append((spelled, combined.zipf_to_f(zipf_frequency(w, "en", wordlist="large")), ""))

    if a.base:
        out = order_apostrophe_twins(out, combined.whitelist(a.base), exact)

    desc = "English (AOSP + wordfreq)" if a.base else "English (wordfreq)"
    combined.write(a.out, out, desc)
    print("%s: %d words" % (a.out, len(out)))


if __name__ == "__main__":
    main()
