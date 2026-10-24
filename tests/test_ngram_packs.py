# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""The n-gram pack builder's word rules (scripts/lm/build-fineweb2-model.py): tiny inputs, no corpus needed."""
import bz2
import importlib.util
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_fineweb2_model", ROOT/"scripts/lm/build-fineweb2-model.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

DICTIONARY = """dictionary=fixture,locale=en
 word=were,f=186,flags=,originalFreq=186
 word=we're,f=106,flags=,originalFreq=106
 word=don't,f=185,flags=,originalFreq=185
 word=whats,f=53,flags=nonword,originalFreq=53
 word=what's,f=150,flags=,originalFreq=150
 word=c’est,f=198,flags=,originalFreq=198
"""


class ApostropheWords(unittest.TestCase):
    def setUp(self):
        self.addCleanup(setattr, builder, "KEY", builder.alpha)
        builder.NONWORDS.clear()

    def build(self, keep):
        builder.KEY = builder.apostrophe_key if keep else builder.alpha
        with tempfile.NamedTemporaryFile("w", suffix=".combined", encoding="utf-8", delete=False) as f:
            f.write(DICTIONARY)
        vocab = builder.load_vocab(f.name)
        words = sorted(vocab)
        return words, builder.token_ids(words, vocab, keep)

    def test_key_keeps_inner_apostrophes_only(self):
        self.assertEqual(builder.apostrophe_key("We’re"), "we're")
        self.assertEqual(builder.apostrophe_key("'tis"), "tis")
        self.assertEqual(builder.apostrophe_key("states'"), "states")
        self.assertEqual(builder.alpha("We're"), "were")

    def test_letters_only_packs_merge_spellings(self):
        words, ids = self.build(False)
        self.assertNotIn("we're", words)
        self.assertEqual(builder.word_id("we're", ids), 0)   # letters-only text never has the apostrophe form
        self.assertIn("cest", words)

    def test_apostrophe_packs_keep_spellings_apart_and_route_sloppy_text(self):
        words, ids = self.build(True)
        builder.init_worker(ids, set(), 1, builder.apostrophe_key)
        self.assertIn("we're", words)
        self.assertIn("c'est", words)                                      # curly written straight
        self.assertNotEqual(builder.word_id("were", ids), builder.word_id("we're", ids))
        self.assertEqual(builder.word_id("dont", ids), ids["don't"])       # no "dont" in the dictionary
        self.assertEqual(builder.word_id("whats", ids), ids["what's"])     # "whats" is flagged not a word
        self.assertEqual(builder.word_id("zzz", ids), 0)

    def test_tatoeba_tokens_use_the_same_rules(self):
        words, ids = self.build(True)
        with tempfile.NamedTemporaryFile(suffix=".bz2", delete=False) as f:
            f.write(bz2.compress("1\teng\tWe’re here, dont worry.\n10\teng\tTest tenth: never used.\n".encode()))
        stream = builder.tatoeba_stream(f.name, "eng", ids, set())
        self.assertEqual(list(stream)[:3], [ids["we're"], 0, ids["don't"]])


if __name__ == "__main__":
    unittest.main()
