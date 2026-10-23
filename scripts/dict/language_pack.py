#!/usr/bin/env python3
"""Generate normal selectable es/fr/de packs from the checksum-pinned 3.1.1 wheel.

Data only: read the wheel's documented cBpack bins directly (no package code,
network, tokenizer, personal words or model training). Tokens already processed
by wordfreq are filtered to declared Latin alphabets; display forms remain NFC.
This establishes reproducibility, not native-language swipe accuracy.

dictionary_from_counts() is the wheel-free successor (word lists counted from FineWeb-2 + Tatoeba, no CC BY-SA);
see packaging/language-models/README.md, "Word lists counted from open text".
"""
import argparse
import gzip
import hashlib
import json
import pathlib
import shutil
import unicodedata
import sys
import zipfile

# WHY: corpus_dictionary.py sits next to this file; it is loaded by path in tests and by fetch-assets.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
ROOT = pathlib.Path(__file__).resolve().parents[2]
TRANSFORMATION = 'wordfreq-latin-v2'
# WHY a bounded, explicit display-form erratum, not generic German title-casing:
# upstream tokens lose noun case/ß and split French elisions. Preserve source ranks,
# leave ambiguous/unlisted words untouched, and version the changed generated data.
GERMAN_DISPLAY = dict(zip(
    'strasse strassen strassenbahn fussball fuss füsse häuser haus krankenhaus krankenhäuser schule schulen familie familien arbeit jahr jahre zeit kind kinder frau frauen mann männer tag tage'.split(),
    'Straße Straßen Straßenbahn Fußball Fuß Füße Häuser Haus Krankenhaus Krankenhäuser Schule Schulen Familie Familien Arbeit Jahr Jahre Zeit Kind Kinder Frau Frauen Mann Männer Tag Tage'.split()))
FRENCH_ELISIONS = {
    'école': 'l’école', 'homme': 'l’homme', 'amour': 'l’amour', 'année': 'l’année',
    'heure': 'l’heure', 'enfant': 'l’enfant', 'eau': 'l’eau', 'été': 'l’été',
    'histoire': 'l’histoire', 'ami': 'l’ami', 'amie': 'l’amie',
    'accord': 'd’accord', 'abord': 'd’abord',
}
# WHY one id for both sources: the n-gram files are built from web text AND Tatoeba sentences.
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


def read_bins(wheel, language):
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    if sha(pathlib.Path(wheel).read_bytes()) != lock['wordfreq_wheel']['sha256']:
        raise ValueError('wordfreq wheel checksum mismatch')
    # WHY lazy msgpack only: no wordfreq/regex/tokenizer dependency, and never execute wheel contents.
    import msgpack
    with zipfile.ZipFile(wheel) as z:
        data = msgpack.unpackb(gzip.decompress(z.read(f'wordfreq/data/large_{language}.msgpack.gz')), raw=False)
    if data[0] != {'format': 'cB', 'version': 1}:
        raise ValueError('unsupported cBpack header')
    return data[1:]


def dictionary_bytes(bins, language, maximum):
    words = {}
    for centibel, bucket in enumerate(bins):
        for original in bucket:
            word = clean_word(original, language)
            if word is not None and word not in words:
                # Preserve rank using the existing combined-file 0..255 scale; no claim of tuning/quality.
                zipf = 9 - centibel / 100
                words[word] = max(1, min(255, round((zipf - 1) * 255 / 7)))
                if len(words) >= maximum:
                    break
        if len(words) >= maximum:
            break
    if language == 'de':
        words = {GERMAN_DISPLAY.get(w, w): f for w, f in words.items()}
    elif language == 'fr':
        # Borrow only an existing source token's rank; never invent a seed-specific score.
        for base, display in FRENCH_ELISIONS.items():
            if base in words:
                words[display] = words[base]
    header = f'dictionary=wordfreq_{language},locale={language},description=Language,version=2\n'
    return (header + ''.join(f' word={w},f={f}\n' for w, f in words.items())).encode('utf-8')


def dictionary_from_counts(counts_path, language, maximum=100000, min_pages=3, typo_ratio=None, foreign=()):
    """The es/fr/de swipe dictionary counted from FineWeb-2 + Tatoeba (scripts/lm/count-words.py), no wordfreq.

    WHY same layout and 0..255 scale as dictionary_bytes: the engine and the n-gram vocabulary read both alike.
    Casing, German noun capitals, ß and French elisions come straight from the counted text, so the hand-made
    GERMAN_DISPLAY / FRENCH_ELISIONS errata the wordfreq path needed do not apply here.
    """
    import corpus_dictionary
    if typo_ratio is None:
        typo_ratio = corpus_dictionary.LANGUAGE_TYPO_RATIO
    entries = corpus_dictionary.language_entries(counts_path, language, ALPHABETS[language], clean_word,
                                                 maximum, min_pages, typo_ratio, foreign)
    header = f'dictionary=fineweb_{language},locale={language},description=Language,version=3\n'
    return (header + ''.join(f' word={w},f={f}\n' for w, f in entries)).encode('utf-8')


def context_pack_files(language, lock, source):
    """Pinned next-word files for one language (+ credit and licence texts), or {} when not downloaded.

    Every byte is checked against assets.lock.json; a present-but-wrong file is an error, never ignored.
    """
    pins = lock['multilingual_context_models'][language]
    shared = lock['language_files']
    wanted = {f'{language}.ngram': pins['modelSha256'], f'{language}.vocab.tsv': pins['vocabSha256'],
              f'{language}-CONTRIBUTORS.txt': pins['contributorsSha256'],
              shared['odcTerms']['path']: shared['odcTerms']['sha256'],
              shared['notice']['path']: shared['notice']['sha256']}
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


def generate_english_context(encoder_root, cc_license, out):
    """English has no swipe pack of its own (FUTO's files stay in models/); only its next-word files are installed.

    Returns the list of files written; an empty list when the language files were not downloaded.
    """
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    files = context_pack_files('en', lock, pathlib.Path(encoder_root)/'language-files')
    if not files:
        return []
    # WHY CC-BY-SA text too: en.vocab.tsv is derived from wordfreq (share-alike), like the other packs' dictionaries.
    cc = pathlib.Path(cc_license).read_bytes()
    if sha(cc) != lock['cc_license_source']['sha256']:
        raise ValueError('CC-BY-SA license checksum mismatch')
    files['CC-BY-SA-4.0.txt'] = cc
    for name, data in files.items():
        if (out/name).exists() and (out/name).read_bytes() != data:
            raise ValueError(f'immutable pack output already differs: {out/name}')
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        if not (out/name).exists():
            (out/name).write_bytes(data)
    return sorted(files)


def generate(wheel, language, encoder_root, cc_license, out, maximum=100000):
    if language not in ALPHABETS or maximum <= 0:
        raise ValueError('unsupported language or nonpositive maximum')
    # WHY an explicit storage gate: never consume the reserved workspace free-space floor.
    if shutil.disk_usage(out.parent).free < 21 * 1024**3:
        raise ValueError('less than 21 GiB free; preserve the 20 GiB floor')
    lock = json.loads((ROOT/'packaging/assets.lock.json').read_text())
    cc = pathlib.Path(cc_license).read_bytes()
    if sha(cc) != lock['cc_license_source']['sha256']:
        raise ValueError('CC-BY-SA license checksum mismatch')
    dictionary = dictionary_bytes(read_bins(wheel, language), language, maximum)
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
    with zipfile.ZipFile(wheel) as z:
        metadata = z.read('wordfreq-3.1.1.dist-info/METADATA')
        apache = z.read('wordfreq-3.1.1.dist-info/LICENSE.txt')
    # Full upstream metadata retains source attribution, SUBTLEX conditions and citation, not only a license ID.
    license_bytes = (b'Language derived wordfreq 3.1.1 data. Transformation: '+TRANSFORMATION.encode()+
        b'; NFC, declared alphabet/token filtering, frequency scaling; bounded common spelling, noun-case and elision errata. Data CC-BY-SA-4.0.\n'+cc+b'\n'+metadata+b'\n'+apache)
    # WHY optional: the n-gram files come from a separate pinned download (assets/language-files). A build
    # that does not have them still yields a working swipe pack, just without next-word suggestions.
    context_files = context_pack_files(language, lock, pathlib.Path(encoder_root)/'language-files')
    files = {f'{language}.combined': dictionary, f'{language}-LICENSE.txt': license_bytes,
             'encoder.pte': encoder, 'metadata.json': metadata_bytes, 'FUTO-MODEL-LICENSE.md': terms,
             'CC-BY-SA-4.0.txt': cc,
             **context_files}
    def asset(name, compatibility):
        data = files[name]
        result = {'path': name, 'sha256': sha(data), 'size': len(data), 'compatibility': compatibility}
        if name=='encoder.pte':
            result['metadataSha256']=sha(metadata_bytes)
        return result
    manifest = {
        'schema': 1, 'id': f'wordfreq-{language}', 'version': '3.1.1-v3',
        'language': language, 'locales': [language, f'{language}-*'],
        'layouts': ['qwerty', 'azerty', 'qwertz'], 'modes': ['Latin'],
        'scripts': ['Latn'], 'futoRevision': '1b13f2c85d6b347f6ea3fbc4b3aaf01fce42429a',
        'executorchRevision': '0b0e2c5cdd67c8b4396a46ea1d1aa72ffb0128d7',
        'compatibility': 'swipekb-futo-v1', 'textRulesVersion': 'unicode-latin-v1',
        'transformationVersion': TRANSFORMATION,
        'license': {'id': 'CC-BY-SA-4.0', 'path': f'{language}-LICENSE.txt',
                    'sha256': sha(license_bytes), 'termsPath': 'CC-BY-SA-4.0.txt', 'termsSha256': sha(cc), 'attribution': 'Robyn Speer / wordfreq 3.1.1; source notices in license file'},
        'encoderLicense': {'path': 'FUTO-MODEL-LICENSE.md', 'sha256': sha(terms)},
        'source': lock['wordfreq_wheel'], 'modelRevision': lock['model_revision'],
        'assets': {'encoder': asset('encoder.pte', 'layout-agnostic'),
                   'dictionary': asset(f'{language}.combined', language)},
    }
    if context_files:
        # WHY schema 2 only with the files: the profile loader treats schema 1 as "no next-word pack".
        manifest['schema'] = 2
        manifest['contextFormat'] = 'swk-ngram-v2'
        manifest['version'] = '3.1.1-v4-language-files-2026-10-22'
        manifest['contextLicense'] = context_license(language, lock, context_files)
        manifest['assets']['contextModel'] = {'path':f'{language}.ngram','sha256':sha(context_files[f'{language}.ngram']),
            'size':len(context_files[f'{language}.ngram']),'compatibility':f'{language}:context-ngram-v1'}
        manifest['assets']['contextVocab'] = {'path':f'{language}.vocab.tsv','sha256':sha(context_files[f'{language}.vocab.tsv']),
            'size':len(context_files[f'{language}.vocab.tsv']),'compatibility':f'{language}:context-ngram-v1'}
    encoded = (json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2)+'\n').encode()
    files[f'{language}.json'] = encoded
    # WHY check every conflict before any write: changing pack format/limits must not partially mutate
    # an immutable existing pack. Publish the manifest last, with no timestamps or rewrites on rerun.
    for name, data in files.items():
        path = out/name
        if path.exists() and path.read_bytes()!=data:
            raise ValueError(f'immutable pack output already differs: {path}')
    out.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = out/name
        if not path.exists():
            path.write_bytes(data)
    return len(dictionary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wheel', type=pathlib.Path, required=True)
    parser.add_argument('--language', choices=ALPHABETS)
    # WHY a separate switch: English reuses FUTO's swipe files; only its next-word files are installed from here.
    parser.add_argument('--english-context', action='store_true')
    parser.add_argument('--encoder-root', type=pathlib.Path, required=True)
    parser.add_argument('--cc-license', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--max', type=int, default=100000)
    args = parser.parse_args()
    if args.english_context:
        print('English next-word files:', generate_english_context(args.encoder_root, args.cc_license, args.out) or 'not downloaded')
        return
    if not args.language:
        parser.error('--language or --english-context required')
    print(f'Language {args.language}: {generate(args.wheel,args.language,args.encoder_root,args.cc_license,args.out,args.max)} dictionary bytes; quality UNVALIDATED')


if __name__ == '__main__':
    main()
