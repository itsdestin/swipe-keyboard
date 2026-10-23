#!/usr/bin/env python3
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
PACK_VERSION = '4.0.0-language-files-2026-10-22-open-words'
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


def generate_english_context(encoder_root, out):
    """English has no swipe pack of its own (FUTO's files stay in models/, its dictionary in dict/); only its
    next-word files are installed.

    Returns the list of files written; an empty list when the language files were not downloaded.
    """
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    files = context_pack_files('en', lock, pathlib.Path(encoder_root)/'language-files')
    if not files:
        return []
    write_immutable(out, files)
    return sorted(files)


def generate(language, encoder_root, out):
    if language not in ALPHABETS:
        raise ValueError('unsupported language')
    # WHY an explicit storage gate: never consume the reserved workspace free-space floor.
    if shutil.disk_usage(out.parent).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free; preserve the 20 GiB floor')
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    # The dictionary now comes from the same pinned download as the n-gram files, so it is required.
    context_files = context_pack_files(language, lock, pathlib.Path(encoder_root)/'language-files')
    if not context_files:
        raise ValueError(f'language files for {language} not downloaded (run scripts/fetch-assets.py)')
    dictionary_name = lock['language_dictionaries'][language]['path']
    dictionary = context_files[dictionary_name]
    model_entry = next(a for a in lock['assets'] if a['path']=='models/honorable_sturgeon/model_fp32.pte')
    terms_entry = next(a for a in lock['assets'] if a['path']=='models/FUTO-MODEL-LICENSE.md')
    metadata_entry = next(a for a in lock['assets'] if a['path']=='models/honorable_sturgeon/metadata.json')
    metadata_bytes = (pathlib.Path(encoder_root)/metadata_entry['path']).read_bytes()
    if sha(metadata_bytes)!=metadata_entry['sha256']:
        raise ValueError('encoder metadata checksum mismatch')
    encoder = (pathlib.Path(encoder_root)/model_entry['path']).read_bytes()
    terms = (pathlib.Path(encoder_root)/terms_entry['path']).read_bytes()
    if sha(encoder)!=model_entry['sha256'] or sha(terms)!=terms_entry['sha256']:
        raise ValueError('encoder/license checksum mismatch')
    files = {'encoder.pte': encoder, 'metadata.json': metadata_bytes, 'FUTO-MODEL-LICENSE.md': terms, **context_files}
    def asset(name, compatibility):
        data = files[name]
        result = {'path': name, 'sha256': sha(data), 'size': len(data), 'compatibility': compatibility}
        if name=='encoder.pte':
            result['metadataSha256']=sha(metadata_bytes)
        return result
    shared = lock['language_files']
    notice_name = shared['notice']['path']
    manifest = {
        # WHY schema 2 always: the profile loader treats schema 1 as "no next-word pack", and these files always ship together.
        'schema': 2, 'id': f'fineweb-{language}', 'version': PACK_VERSION,
        'language': language, 'locales': [language, f'{language}-*'],
        'layouts': ['qwerty', 'azerty', 'qwertz'], 'modes': ['Latin'],
        'scripts': ['Latn'], 'futoRevision': '1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a',
        'executorchRevision': '0b0e2c5cdd67c8b4396a46ea1d1aa72ffb0128d7',
        'compatibility': 'swipekb-futo-v1', 'textRulesVersion': 'unicode-latin-v1',
        'transformationVersion': TRANSFORMATION,
        # The word list is counted from the same web text and Tatoeba sentences as the n-gram files; the notice
        # describes it, ODC-By and the Tatoeba terms are the texts that must travel with it.
        'license': {'id': CONTEXT_LICENSE_ID, 'path': notice_name, 'sha256': sha(files[notice_name]),
                    'termsPath': shared['odcTerms']['path'], 'termsSha256': sha(files[shared['odcTerms']['path']]),
                    'tatoebaTermsPath': 'TATOEBA-CC-BY-2.0-FR.txt', 'tatoebaTermsSha256': sha(files['TATOEBA-CC-BY-2.0-FR.txt']),
                    'attribution': 'FineWeb / FineWeb-2 (Hugging Face; Common Crawl) under ODC-By 1.0 and Tatoeba contributors under CC BY 2.0 FR; source notices in the notice file'},
        'encoderLicense': {'path': 'FUTO-MODEL-LICENSE.md', 'sha256': sha(terms)},
        'source': lock['word_list_source'], 'modelRevision': lock['model_revision'],
        'assets': {'encoder': asset('encoder.pte', 'layout-agnostic'),
                   'dictionary': asset(dictionary_name, language),
                   'contextModel': {'path': f'{language}.ngram', 'sha256': sha(files[f'{language}.ngram']),
                                    'size': len(files[f'{language}.ngram']), 'compatibility': f'{language}:context-ngram-v1'},
                   'contextVocab': {'path': f'{language}.vocab.tsv', 'sha256': sha(files[f'{language}.vocab.tsv']),
                                    'size': len(files[f'{language}.vocab.tsv']), 'compatibility': f'{language}:context-ngram-v1'}},
        'contextFormat': 'swk-ngram-v2',
        'contextLicense': context_license(language, lock, context_files),
    }
    encoded = (json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2)+'\n').encode()
    files[f'{language}.json'] = encoded
    # WHY check every conflict before any write: changing pack format/limits must not partially mutate
    # an immutable existing pack. Publish the manifest last, with no timestamps or rewrites on rerun.
    write_immutable(out, files)
    return len(dictionary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--language', choices=ALPHABETS)
    # WHY a separate switch: English reuses FUTO's swipe files; only its next-word files are installed from here.
    parser.add_argument('--english-context', action='store_true')
    parser.add_argument('--encoder-root', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.english_context:
        print('English next-word files:', generate_english_context(args.encoder_root, args.out) or 'not downloaded')
        return
    if not args.language:
        parser.error('--language or --english-context required')
    print(f'Language {args.language}: {generate(args.language, args.encoder_root, args.out)} dictionary bytes; quality UNVALIDATED')


if __name__ == '__main__':
    main()
