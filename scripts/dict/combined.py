"""Shared helpers for reading and writing AOSP `.combined` dictionaries (the format FUTO's engine loads).

WHY a tiny shared module: every candidate-dictionary script reads and writes the same text format, and the
engine silently skips lines it cannot parse, so one careful writer is safer than several copies.
"""
import re
import os

_WORD = re.compile(r"^ word=([^,]+),f=(\d+)(?:,flags=([^,]*))?")


_SHORTCUT = re.compile(r"^  shortcut=([^,]+),f=whitelist")


def read(path):
    """Returns [(word, freq, flags)] in file order. Shortcut/whitelist lines are dropped."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = _WORD.match(line)
            if m:
                out.append((m.group(1), int(m.group(2)), m.group(3) or ""))
    return out


def not_words(path):
    """The words AOSP marks `not_a_word=true` ("im", "alot", "teh"-style typos, f=0).

    WHY a separate reader: read() keeps only the word, frequency and flags, so it used to turn these lines into
    ordinary f=0 words ("im" became a real word). AOSP lists them only so its autocorrect can replace them
    ("im" -> "I'm"); they are not words a swipe should ever type.
    """
    out = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = _WORD.match(line)
            if m and "not_a_word=true" in line:
                out.add(m.group(1))
    return out


def whitelist(path):
    """AOSP's "always replace" pairs: {typed word: replacement}, e.g. "lets" -> "let's", "ill" -> "I'll".

    WHY: a whitelist line follows the word it replaces. AOSP's own keyboard uses these to prefer "let's" over
    "lets", but NOT "it's" over "its"; that is exactly the apostrophe preference the swipe dictionary needs.
    """
    out, last = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = _WORD.match(line)
            if m:
                last = m.group(1)
                continue
            s = _SHORTCUT.match(line)
            if s and last is not None:
                out[last] = s.group(1)
    return out


def write(path, entries, description):
    """Writes [(word, freq, flags)] most frequent first, in the AOSP layout."""
    entries = sorted(entries, key=lambda e: (-e[1], e[0]))
    with open(path, "w", encoding="utf-8") as f:
        f.write("dictionary=main:en_us,locale=en_US,description=%s,date=%d,version=54\n"
                % (description, int(os.environ.get("SOURCE_DATE_EPOCH", "0"))))
        for word, freq, flags in entries:
            f.write(" word=%s,f=%d,flags=%s,originalFreq=%d\n" % (word, freq, flags, freq))


def zipf_to_f(zipf):
    """Maps a wordfreq Zipf value (log10 of uses per billion words) to AOSP's 0-255 `f` scale.

    WHY these numbers: fitted on the ~86k lowercase words both lists share. AOSP's median `f` rises about
    27.5 per Zipf step ("the" 7.7 -> 211, AOSP 222; a Zipf-3 word -> 82, AOSP median 81), so the same
    word gets about the same weight whichever list it came from.
    """
    return max(1, min(255, round(27.5 * zipf - 1)))
