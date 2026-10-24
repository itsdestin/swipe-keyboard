# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Word counting and the corpus-built dictionaries (card #170 Phase 1): tiny inputs, no corpus needed."""
import bz2
import importlib.util
import pathlib
import sys
import tempfile
import unittest
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts/dict'))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT/path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


counting = load('count_words', 'scripts/lm/count-words.py')
language_pack = load('language_pack_for_corpus', 'scripts/dict/language_pack.py')
corpus = load('corpus_dictionary_test', 'scripts/dict/corpus_dictionary.py')


def count_text(*pages, banned=frozenset()):
    occ, pgs, spell_all, spell_inner = Counter(), Counter(), Counter(), Counter()
    for text in pages:
        counting.count_page(text, banned, occ, pgs, spell_all, spell_inner)
    return occ, pgs, spell_all, spell_inner


class CountWords(unittest.TestCase):
    def test_counts_pages_and_ignores_sentence_start_capitals(self):
        # "Haus" is a noun everywhere; "Und" is only capitalised because it starts the sentence.
        occ, pages, spell_all, spell_inner = count_text(
            "Und das Haus ist gross. Das Haus steht da.", "Und wir sehen das Haus.")
        self.assertEqual(occ['haus'], 3)
        self.assertEqual(pages['haus'], 2)   # distinct pages, not occurrences
        self.assertEqual(occ['und'], 2)
        self.assertEqual(counting.choose_spelling({k: v for k, v in spell_inner.items() if k.lower() == 'haus'},
                                                  {k: v for k, v in spell_all.items() if k.lower() == 'haus'}), 'Haus')
        self.assertNotIn('Und', spell_inner)  # never inside a sentence here

    def test_typographic_apostrophe_and_nfc_fold_into_one_word(self):
        occ, *_ = count_text("Il dit l\u2019\u00e9cole est belle. Il dit l'\u00e9cole est belle.")
        self.assertEqual(occ["l'\u00e9cole"], 2)

    def test_words_glued_to_digits_or_addresses_are_not_words(self):
        occ, *_ = count_text("On the 5th of May mp3 files at example.com and h2o, e.g. the u.s. mail user@host.net plan.")
        for junk in ('th', 'mp', 'example', 'com', 'h', 'e', 'g', 'host', 'net', 'user'):
            self.assertNotIn(junk, occ)
        for word in ('on', 'the', 'may', 'files', 'plan'):
            self.assertIn(word, occ)
        self.assertEqual(occ['the'], 2)   # "e.g. the" and "On the" both free-standing

    def test_sentence_final_word_before_a_space_still_counts(self):
        occ, *_ = count_text("Es una casa. Es una casa grande.")
        self.assertEqual(occ['casa'], 2)

    def test_numbers_and_one_word_sentences_do_not_count(self):
        occ, *_ = count_text("Hola. 123 456 hola mundo.")
        self.assertEqual(occ['hola'], 1)      # "Hola." alone is one word long and skipped
        self.assertNotIn('123', occ)

    def test_sentences_sharing_an_eight_word_run_with_a_test_set_are_dropped(self):
        test_run = counting.runs([counting.alpha(t) for t in counting.WORD.findall(
            "uno dos tres cuatro cinco seis siete ocho")])
        occ, *_ = count_text("uno dos tres cuatro cinco seis siete ocho nueve. gato perro casa.", banned=test_run)
        self.assertNotIn('nueve', occ)
        self.assertEqual(occ['gato'], 1)

    def test_tatoeba_training_tenths_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)/'t.tsv.bz2'
            rows = ["1\tspa\tel gato duerme", "10\tspa\tel perro corre", "11\tfra\tle chat dort"]
            path.write_bytes(bz2.compress(('\n'.join(rows)+'\n').encode()))
            occ, pages, spell_all, spell_inner = Counter(), Counter(), Counter(), Counter()
            words = counting.tatoeba_counts(str(path), 'spa', frozenset(), occ, pages, spell_all, spell_inner)
        self.assertEqual(words, 3)
        self.assertIn('gato', occ)
        self.assertNotIn('perro', occ)   # id % 10 == 0 is the held-out test tenth
        self.assertNotIn('chat', occ)    # other language


def write_counts(path, total, rows):
    path.write_text(f"# total_words={total} language=xx\n" +
                    ''.join(f"{k}\t{c}\t{p}\t{s}\n" for k, c, p, s in rows), encoding='utf-8')


class TypoFilter(unittest.TestCase):
    TOTAL = 300_000_000

    def rows(self):
        return [('memorias', 1000, 900, 'memorias'), ('memorais', 20, 15, 'memorais'), ('memorias2', 5, 5, 'x')]

    def test_edit_distance_one_set(self):
        edits = set(corpus.edits1('ab', 'ab'))
        for typo in ('a', 'b', 'bb', 'aab', 'aa', 'ba', 'abb', 'aba'):
            self.assertIn(typo, edits)

    def test_rare_neighbour_of_a_much_commoner_word_is_a_typo(self):
        typo = corpus.make_typo_test(self.rows(), 'abcdefghijklmnopqrstuvwxyz', 20, self.TOTAL)
        self.assertTrue(typo('memorais'))     # swapped letters, 50x rarer
        self.assertFalse(typo('memorias'))
        self.assertFalse(corpus.make_typo_test(self.rows(), 'abcdefghijklmnopqrstuvwxyz', 0, self.TOTAL)('memorais'))

    def test_short_words_are_never_typos(self):
        rows = [('de', 100000, 9000, 'de'), ('te', 3000, 2000, 'te')]
        self.assertFalse(corpus.make_typo_test(rows, 'abcdefghijklmnopqrstuvwxyz', 20, self.TOTAL)('te'))

    def test_common_words_next_to_commoner_ones_are_kept(self):
        # "perro" (dog) is one letter from "pero" (but): used far too often to be a misspelling.
        rows = [('pero', 3_000_000, 90000, 'pero'), ('perro', 60_000, 9000, 'perro')]
        self.assertFalse(corpus.make_typo_test(rows, 'abcdefghijklmnopqrstuvwxyz', 20, self.TOTAL)('perro'))

    def test_accented_word_is_not_a_typo_of_its_plain_form_but_the_plain_form_is(self):
        rows = [('segura', 100000, 9000, 'segura'), ('segurá', 100, 90, 'segurá')]
        typo = corpus.make_typo_test(rows, 'abcdefghijklmnopqrstuvwxyzá', 20, self.TOTAL)
        self.assertFalse(typo('segurá'))
        rows = [('educación', 100000, 9000, 'educación'), ('educacion', 100, 90, 'educacion')]
        self.assertTrue(corpus.make_typo_test(rows, 'abcdefghijklmnopqrstuvwxyzó', 20, self.TOTAL)('educacion'))


class LanguageDictionary(unittest.TestCase):
    def build(self, language, rows, total=300_000_000, foreign=(), **kw):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)/'c.tsv'
            write_counts(path, total, rows)
            return language_pack.dictionary_from_counts(str(path), language, foreign=foreign, **kw).decode()

    def test_frequency_scale_junk_filters_and_display_forms(self):
        text = self.build('de', [
            ('der', 50000, 9000, 'der'), ('haus', 4000, 3000, 'Haus'), ('strasse', 10, 9, 'Straße'),
            ('https', 3000, 2000, 'https'), ('seltenwort', 5, 2, 'seltenwort'),      # 2 pages -> junk
            ('x', 900, 800, 'x'), ('привет', 500, 400, 'привет'), ('kaufen', 300, 200, 'kaufen')])
        self.assertIn(' word=der,f=', text)
        self.assertIn(' word=Haus,f=', text)          # noun capital comes from the counted spelling
        self.assertIn(' word=Straße,f=', text)
        for gone in ('https', 'seltenwort', 'word=x,', 'привет'):
            self.assertNotIn(gone, text)
        f = {line.split('word=')[1].split(',')[0]: int(line.split('f=')[1]) for line in text.splitlines()[1:]}
        self.assertGreater(f['der'], f['Haus'])
        self.assertGreater(f['Haus'], f['kaufen'])
        self.assertTrue(all(1 <= v <= 255 for v in f.values()))
        self.assertEqual(f['der'], round((__import__('math').log10(50000/3e8*1e9) - 1)*255/7))

    def test_french_elision_uses_the_typographic_apostrophe_and_maximum_applies(self):
        text = self.build('fr', [("l'école", 900, 800, "l'école"), ('maison', 800, 700, 'maison'), ('chat', 700, 600, 'chat')],
                          maximum=2)
        self.assertIn(' word=l’école,f=', text)
        self.assertNotIn('chat', text)

    def test_spanish_keeps_real_one_letter_words_only(self):
        text = self.build('es', [('y', 900, 800, 'y'), ('b', 800, 700, 'b'), ('casa', 700, 600, 'casa')])
        self.assertIn(' word=y,', text)
        self.assertNotIn(' word=b,', text)

    def test_words_far_more_typical_of_another_language_are_dropped(self):
        with tempfile.TemporaryDirectory() as tmp:
            other = pathlib.Path(tmp)/'en.tsv'
            write_counts(other, 300_000_000, [('presentation', 5000, 4000, 'presentation'), ('hotel', 100, 90, 'hotel')])
            text = self.build('es', [('presentation', 50, 40, 'presentation'), ('hotel', 100, 90, 'hotel'),
                                     ('casa', 700, 600, 'casa')], foreign=[str(other)])
        self.assertNotIn('presentation', text)
        self.assertIn(' word=hotel,', text)

    def test_deterministic(self):
        rows = [('der', 50000, 9000, 'der'), ('haus', 4000, 3000, 'Haus')]
        self.assertEqual(self.build('de', rows), self.build('de', rows))


class EnglishDictionary(unittest.TestCase):
    AOSP = ("dictionary=main:en_us,locale=en_US,description=x,date=1,version=54\n"
            " word=the,f=222,flags=,originalFreq=222\n"
            " word=its,f=150,flags=,originalFreq=150\n word=it's,f=140,flags=,originalFreq=140\n"
            " word=damn,f=100,flags=,originalFreq=100\n  not_a_word=true\n"
            " word=alot,f=0,flags=,originalFreq=0,not_a_word=true\n"
            " word=a,f=200,flags=,originalFreq=200\n")

    def test_aosp_base_is_kept_and_only_missing_words_are_added(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = pathlib.Path(tmp)/'base.combined'
            base.write_text(self.AOSP, encoding='utf-8')
            counts = pathlib.Path(tmp)/'en.tsv'
            write_counts(counts, 300_000_000, [
                ('the', 60000, 9000, 'the'), ('zebra', 400, 300, 'zebra'), ('zebre', 5, 4, 'zebre'),
                ('london', 300, 250, 'London'), ('b', 500, 400, 'b'), ('alot', 900, 800, 'alot'),
                ('rare', 9, 2, 'rare'), ('th', 9000, 5000, 'th'), ('pp', 800, 700, 'pp')])
            entries = {w: (f, fl) for w, f, fl in corpus.english_entries(str(counts), str(base), [], 1000)}
        self.assertEqual(entries['the'], (222, ''))             # base frequency untouched
        self.assertIn('zebra', entries)
        self.assertNotIn('zebre', entries)                      # one typo from a 80x commoner word
        self.assertIn('London', entries)                        # casing from the counted text
        self.assertNotIn('b', entries)
        self.assertNotIn('alot', entries)                       # AOSP "not a word" stays gone
        self.assertNotIn('rare', entries)                       # two pages only
        self.assertNotIn('th', entries)                         # fragments of "5th" and the like
        self.assertNotIn('pp', entries)
        self.assertGreater(entries['its'][0], entries["it's"][0])   # apostrophe twin ordering still applies


if __name__ == '__main__':
    unittest.main()
