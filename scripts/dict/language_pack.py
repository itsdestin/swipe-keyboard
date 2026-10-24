#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Generate the selectable es/fr/de packs (and English's next-word files) from the checksum-pinned language files.

Data only: no package code, network, tokenizer, personal words or model training. The swipe dictionaries, n-gram
models, word lists and licence texts come from the pinned GitHub release in assets.lock.json (downloaded by
scripts/fetch-assets.py into <assets>/language-files); every byte is checked against the lock before it is copied.
The word lists are counted from FineWeb / FineWeb-2 and Tatoeba (scripts/lm/count-words.py,
scripts/dict/corpus_dictionary.py), so only attribution terms apply: ODC-By 1.0 and CC BY 2.0 FR (English also keeps
AOSP's Apache-2.0 word list). This establishes reproducibility, not native-language swipe accuracy.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import unicodedata

# WHY: corpus_dictionary.py sits next to this file; it is loaded by path in tests and by fetch-assets.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
ROOT = pathlib.Path(__file__).resolve().parents[2]
TRANSFORMATION = 'fineweb-counts-v1'
PACK_VERSION = '4.1.0-language-files-2026-10-23-apostrophes'
# WHY one id for both sources: the dictionaries, word lists and n-gram files are built from web text AND Tatoeba sentences.
CONTEXT_LICENSE_ID = 'ODC-By-1.0 AND CC-BY-2.0-FR'
ALPHABETS = {
    'es': 'abcdefghijklmnopqrstuvwxyzáéíóúüñ',
    'fr': 'abcdefghijklmnopqrstuvwxyzàâéèêëîïôùûüÿçœæ',
    'de': 'abcdefghijklmnopqrstuvwxyzäöüß',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def clean_word(word, language):
    word = unicodedata.normalize('NFC', word)
    if not 1 <= len(word) <= 48 or not word[0].isalpha() or not word[-1].isalpha():
        return None
    previous_letter = False
    for c in word:
        if c.lower() in ALPHABETS[language] and c.isalpha():
            previous_letter = True
        elif c in "'’-" and previous_letter:
            previous_letter = False
        else:
            return None
    return word if previous_letter else None


def dictionary_from_counts(counts_path, language, maximum=100000, min_pages=3, typo_ratio=None, foreign=()):
    """The es/fr/de swipe dictionary counted from FineWeb-2 + Tatoeba (scripts/lm/count-words.py).

    This is how the pinned `<lang>.combined` files were made (scripts/dict/build-corpus-dictionaries.sh); the
    installer downloads those files rather than rebuilding them. Casing, German noun capitals, ß and French
    elisions come straight from the counted text.
    """
    import corpus_dictionary
    if typo_ratio is None:
        typo_ratio = corpus_dictionary.LANGUAGE_TYPO_RATIO
    entries = corpus_dictionary.language_entries(counts_path, language, ALPHABETS[language], clean_word,
                                                 maximum, min_pages, typo_ratio, foreign)
    header = f'dictionary=fineweb_{language},locale={language},description=Language,version=3\n'
    return (header + ''.join(f' word={w},f={f}\n' for w, f in entries)).encode('utf-8')


def context_pack_files(language, lock, source):
    """Pinned language files for one language (+ credit and licence texts), or {} when not downloaded.

    es/fr/de also carry their swipe dictionary; English's dictionary installs separately (dict/en_swipe.combined),
    but its Apache-2.0 text (AOSP's word list) travels with its n-gram files.
    Every byte is checked against assets.lock.json; a present-but-wrong file is an error, never ignored.
    """
    pins = lock['multilingual_context_models'][language]
    shared = lock['language_files']
    wanted = {f'{language}.ngram': pins['modelSha256'], f'{language}.vocab.tsv': pins['vocabSha256'],
              f'{language}-CONTRIBUTORS.txt': pins['contributorsSha256'],
              shared['odcTerms']['path']: shared['odcTerms']['sha256'],
              shared['notice']['path']: shared['notice']['sha256']}
    if language == 'en':
        wanted[shared['apacheTerms']['path']] = shared['apacheTerms']['sha256']
    else:
        wanted[lock['language_dictionaries'][language]['path']] = lock['language_dictionaries'][language]['sha256']
    present = {name: (source/name).is_file() for name in wanted}
    if not any(present.values()):
        return {}
    if not all(present.values()):
        raise ValueError('incomplete language files in ' + str(source) + ': ' + ', '.join(n for n, ok in present.items() if not ok))
    files = {}
    for name, digest in wanted.items():
        data = (source/name).read_bytes()
        if sha(data) != digest:
            raise ValueError('language file pin mismatch: ' + name)
        files[name] = data
    tatoeba = (ROOT/'packaging/language-models/TATOEBA-CC-BY-2.0-FR.txt').read_bytes()
    if sha(tatoeba) != lock['tatoeba_license_notice_sha256']:
        raise ValueError('Tatoeba licence notice pin mismatch')
    files['TATOEBA-CC-BY-2.0-FR.txt'] = tatoeba
    # WHY optional, but never wrong: the neural next-word model is an add-on to the n-gram. Absent = pack without it;
    # present with a different hash = error (same rule as every other pinned file).
    nwp_pin = lock.get('trained_models', {}).get('nextWord', {}).get(language)
    if nwp_pin and (source/nwp_pin['path']).is_file():
        data = (source/nwp_pin['path']).read_bytes()
        if sha(data) != nwp_pin['sha256'] or len(data) != nwp_pin['bytes']:
            raise ValueError('language file pin mismatch: ' + nwp_pin['path'])
        files[nwp_pin['path']] = data
        notice = lock['trained_models']['notice']
        notice_data = (source/notice['path']).read_bytes()
        if sha(notice_data) != notice['sha256']:
            raise ValueError('language file pin mismatch: ' + notice['path'])
        files[notice['path']] = notice_data
    return files


def context_license(language, lock, files):
    """Licence metadata for the n-gram files: web text (ODC-By) AND Tatoeba sentences (CC BY 2.0 FR)."""
    shared = lock['language_files']
    return {'id': CONTEXT_LICENSE_ID,
        'attribution': 'FineWeb text under ODC-By 1.0 (Hugging Face; Common Crawl) and Tatoeba contributors under CC BY 2.0 FR; '
                       'Tatoeba authors are listed in the CONTRIBUTORS file',
        'attributionPath': f'{language}-CONTRIBUTORS.txt', 'attributionSha256': sha(files[f'{language}-CONTRIBUTORS.txt']),
        'termsPath': 'TATOEBA-CC-BY-2.0-FR.txt', 'termsSha256': sha(files['TATOEBA-CC-BY-2.0-FR.txt']),
        'webTermsPath': shared['odcTerms']['path'], 'webTermsSha256': sha(files[shared['odcTerms']['path']]),
        'noticePath': shared['notice']['path'], 'noticeSha256': sha(files[shared['notice']['path']]),
        'sourceUrl': context_source_url(lock, language), 'termsUrl': 'https://tatoeba.org/en/terms_of_use'}


def context_source_url(lock, language):
    return lock['language_files']['baseUrl'] + f'{language}.ngram'


def write_immutable(out, files):
    """Writes every file or none: an existing file that differs aborts before anything is changed."""
    for name, data in files.items():
        if (out/name).exists() and (out/name).read_bytes() != data:
            raise ValueError(f'immutable pack output already differs: {out/name}')
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        if not (out/name).exists():
            (out/name).write_bytes(data)


def generate_english_context(assets_root, out):
    """English has no pack manifest of its own (its dictionary installs as dict/en_swipe.combined); only its
    next-word files are installed here.

    Returns the list of files written; an empty list when the language files were not downloaded.
    """
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    files = context_pack_files('en', lock, pathlib.Path(assets_root)/'language-files')
    if not files:
        return []
    write_immutable(out, files)
    return sorted(files)


def generate_shared(assets_root, out):
    """The swipe model every language uses, with its notices, beside the packs (the keyboard looks for it there).

    Returns the files written; [] when the model was not downloaded. A present file with another hash is an error.
    """
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    models = lock['trained_models']
    source = pathlib.Path(assets_root)/'language-files'
    swipe = models['swipe']
    name = pathlib.Path(swipe['path']).name
    if not (source/name).is_file():
        return []
    files = {}
    for entry, file in ((swipe, name), (models['notice'], models['notice']['path']), (models['swipeDataTerms'], models['swipeDataTerms']['path'])):
        data = (source/file).read_bytes()
        if sha(data) != entry['sha256'] or ('bytes' in entry and len(data) != entry['bytes']):
            raise ValueError('trained model pin mismatch: ' + file)
        files[file] = data
    write_immutable(out, files)
    return sorted(files)


def generate(language, assets_root, out):
    if language not in ALPHABETS:
        raise ValueError('unsupported language')
    # WHY an explicit storage gate: never consume the reserved workspace free-space floor.
    if shutil.disk_usage(out.parent).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free; preserve the 20 GiB floor')
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    # The dictionary comes from the same pinned download as the n-gram files, so it is required.
    files = context_pack_files(language, lock, pathlib.Path(assets_root)/'language-files')
    if not files:
        raise ValueError(f'language files for {language} not downloaded (run scripts/fetch-assets.py)')
    dictionary_name = lock['language_dictionaries'][language]['path']
    def asset(name, compatibility):
        data = files[name]
        return {'path': name, 'sha256': sha(data), 'size': len(data), 'compatibility': compatibility}
    shared = lock['language_files']
    notice_name = shared['notice']['path']
    manifest = {
        # WHY schema 3 / swipekb-open-v1: the pack holds only the project's own open files (word list, n-gram,
        # next-word model, licences); the keyboard refuses older packs built for another swipe engine.
        'schema': 3, 'id': f'fineweb-{language}', 'version': PACK_VERSION,
        'language': language, 'locales': [language, f'{language}-*'],
        'layouts': ['qwerty', 'azerty', 'qwertz'], 'modes': ['Latin'],
        'scripts': ['Latn'], 'compatibility': 'swipekb-open-v1', 'textRulesVersion': 'unicode-latin-v1',
        'transformationVersion': TRANSFORMATION,
        # The word list is counted from the same web text and Tatoeba sentences as the n-gram files; the notice
        # describes it, ODC-By and the Tatoeba terms are the texts that must travel with it.
        'license': {'id': CONTEXT_LICENSE_ID, 'path': notice_name, 'sha256': sha(files[notice_name]),
                    'termsPath': shared['odcTerms']['path'], 'termsSha256': sha(files[shared['odcTerms']['path']]),
                    'tatoebaTermsPath': 'TATOEBA-CC-BY-2.0-FR.txt', 'tatoebaTermsSha256': sha(files['TATOEBA-CC-BY-2.0-FR.txt']),
                    'attribution': 'FineWeb / FineWeb-2 (Hugging Face; Common Crawl) under ODC-By 1.0 and Tatoeba contributors under CC BY 2.0 FR; source notices in the notice file'},
        'source': lock['word_list_source'],
        'assets': {'dictionary': asset(dictionary_name, language),
                   'contextModel': asset(f'{language}.ngram', f'{language}:context-ngram-v1'),
                   'contextVocab': asset(f'{language}.vocab.tsv', f'{language}:context-ngram-v1')},
        'contextFormat': 'swk-ngram-v2',
        'contextLicense': context_license(language, lock, files),
    }
    encoded = (json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2)+'\n').encode()
    files[f'{language}.json'] = encoded
    # WHY check every conflict before any write: changing pack format/limits must not partially mutate
    # an immutable existing pack. Publish the manifest last, with no timestamps or rewrites on rerun.
    write_immutable(out, files)
    return len(files[dictionary_name])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--language', choices=ALPHABETS)
    # WHY a separate switch: English has no manifest; only its next-word files are installed from here.
    parser.add_argument('--english-context', action='store_true')
    parser.add_argument('--assets-root', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.english_context:
        print('English next-word files:', generate_english_context(args.assets_root, args.out) or 'not downloaded')
        return
    if not args.language:
        parser.error('--language or --english-context required')
    print(f'Language {args.language}: {generate(args.language, args.assets_root, args.out)} dictionary bytes; quality UNVALIDATED')


if __name__ == '__main__':
    main()
