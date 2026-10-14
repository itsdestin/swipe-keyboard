"""Shared helpers for reading and writing AOSP `.combined` dictionaries (the format FUTO's engine loads).

WHY a tiny shared module: every candidate-dictionary script reads and writes the same text format, and the
engine silently skips lines it cannot parse, so one careful writer is safer than several copies.
"""
import re
import time

_WORD = re.compile(r"^ word=([^,]+),f=(\d+)(?:,flags=([^,]*))?")


def read(path):
    """Returns [(word, freq, flags)] in file order. Shortcut/whitelist lines are dropped."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = _WORD.match(line)
            if m:
                out.append((m.group(1), int(m.group(2)), m.group(3) or ""))
    return out


def write(path, entries, description):
    """Writes [(word, freq, flags)] most frequent first, in the AOSP layout."""
    entries = sorted(entries, key=lambda e: (-e[1], e[0]))
    with open(path, "w", encoding="utf-8") as f:
        f.write("dictionary=main:en_us,locale=en_US,description=%s,date=%d,version=54\n"
                % (description, int(time.time())))
        for word, freq, flags in entries:
            f.write(" word=%s,f=%d,flags=%s,originalFreq=%d\n" % (word, freq, flags, freq))


def zipf_to_f(zipf):
    """Maps a wordfreq Zipf value (log10 of uses per billion words) to AOSP's 0-255 `f` scale.

    WHY these numbers: fitted on the ~86k lowercase words both lists share. AOSP's median `f` rises about
    27.5 per Zipf step ("the" 7.7 -> 211, AOSP 222; a Zipf-3 word -> 82, AOSP median 81), so the same
    word gets about the same weight whichever list it came from.
    """
    return max(1, min(255, round(27.5 * zipf - 1)))
