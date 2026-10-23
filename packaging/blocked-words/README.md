# Words never offered unprompted

`LANG.txt`: one lower-case word per line. The keyboard never shows these as a next-word suggestion before
any letter of the word is typed. Swiping them, and completing them once letters are typed, always works
(Destin, 2026-10-22: "i swipe 'fuck' and others often").

Sources:
- "List of Dirty, Naughty, Obscene, and Otherwise Bad Words" by Shutterstock and contributors,
  https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words (commit 5faf2ba),
  CC BY 4.0 (`CC-BY-4.0.txt`). Single words only; multi-word entries dropped.
- English also: words the Android (AOSP) LatinIME word list flags `offensive`, Apache-2.0.

Changes: lower-cased, NFC, curly apostrophes made straight, and ordinary words removed so they can still
be suggested: es asesinato, drogas, infierno, concha; fr folle; de nackt; en sexual.
