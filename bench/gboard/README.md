# Gboard reference (card #170)

What Gboard suggests for the same sentences our keyboard is scored on, recorded once so every later change
can be compared with it without an Android device: `bench/scoreboard.sh` replays `reference/*.tsv` through
our predictor (`swipekb-nextword-eval --gboard`).

## What was measured (2026-10-22)

- Gboard 14.2 (the copy built into Google's Android 35 x86_64 emulator image), learning from typing off,
  one language at a time (multilingual typing off), Spanish/French/German keyboards.
- 100 sentences per language per set, from our own test sets (`scripts/lm/make-nextword-testsets.py`):
  `everyday` = Common Voice community sentences (CC0), `web` = FineWeb-2 test pages (ODC-BY).
- For every word: the words before it plus its first 0, 1, 2 or 3 letters are put in a Messages text box,
  and the three suggestions are read off the screen (left, middle, right; middle = Gboard's best guess).
- Sentence starts are not scored: Gboard shows shortcut chips there instead of words.
- Once letters are typed, Gboard spends one slot on the typed letters (95–100% of rows), so our fair
  figure there is "right word in our top 2"; before typing, both get 3.
- Check of the fast text-fill method against real key taps: 30 random phrases, 27 gave identical
  suggestions (`validate_taps.py`); the 3 differences were single words with no direction.

Limits: one Gboard version (current Gboard is newer); learning off, so it shows Gboard's out-of-the-box
model, not what it does after weeks of use; 100 sentences per set (~±2 points of noise).

## Files

`reference/LANG-SET.tsv`, one row per query: sentence#, word#, letters typed, text before the word, the
word, Gboard's left / middle / right suggestion.

## Re-recording (only if a newer Gboard is wanted)

Needs the Android SDK emulator with a Google APIs x86_64 image, and `uiautomator2` in a Python venv.
1. Create an AVD, add the es/fr/de Gboard keyboards, turn off personalisation and multilingual typing, and
   turn off the system spell checker.
2. Start one emulator per language, then open the Messages app (`am start -a android.intent.action.SENDTO -d sms:5551234`).
3. `python -I gboard_probe.py LANG SENTENCES OUT.tsv --count 100 --serial emulator-5554`
   (screen positions in the script assume the default 1080×2400 phone profile).
