# Capital letters (English, Spanish, French)

`LANG.tsv` (en, es, fr): the words the keyboard shows with a capital letter: "I", "I'm", "Monday", "Paris",
"Madrid", "iPhone", "NASA". Every other word is shown in lower case; Shift and the start of a sentence still
capitalise as usual. German has no list: its dictionary keeps noun capitals itself.

Used by swiping, completion, other spellings, tap-to-swap and next-word suggestions, in both keyboards and in Gaming
Mode (`swipekb::Capitals`, loaded by `capitalsFor` in `plugin/languagetextrules.h`).

## How the lists are made

`scripts/lm/capitals.py`, from the same open web text as the word lists (FineWeb for English, FineWeb-2 for Spanish
and French; the first ~100 million words of each). A word is listed only when both hold:

1. Inside sentences (never the first word, where everything is capitalised), it is written with a capital at least
   90% of the time, seen at least 50 times.
2. The language's dictionary also gives the capitalised form as the word's most common one.

WHY both: web text capitalises many ordinary words inside names ("Banco Santander", "Hall of Fame", "United
States"), so "banco", "hall" or "united" can look like names in the counts. Spanish "unidos/unidas/unido/unida" and
French "unis/unies/uni/unie/etats" are left out by hand for the same reason (`KEEP_LOWER` in the script): their
dictionaries keep one form per word, so rule 2 cannot catch them.

Format: `word<TAB>Form<TAB>share`, `#` lines are comments; the keyboard reads the first two columns.

Rebuild (on the training machine, with the raw text from the word-list build):

    venv/bin/python -I scripts/lm/capitals.py en assets/dict/en_swipe.combined packaging/capitals/en.tsv \
        --parquet en/raw/fw-train-000.parquet --max-words 100000000

## Licence

Facts counted from FineWeb / FineWeb-2 (Hugging Face, derived from Common Crawl), Open Data Commons Attribution
License 1.0 (ODC-By-1.0), the same terms and credit as the word lists (`packaging/NOTICE`).
