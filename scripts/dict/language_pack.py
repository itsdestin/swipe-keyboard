#!/usr/bin/env python3
"""Generate normal selectable es/fr/de packs from the checksum-pinned 3.1.1 wheel.

Data only: read the wheel's documented cBpack bins directly (no package code,
network, tokenizer, personal words or model training). Tokens already processed
by wordfreq are filtered to declared Latin alphabets; display forms remain NFC.
This establishes reproducibility, not native-language swipe accuracy.
"""
import argparse
import gzip
import hashlib
import json
import pathlib
import shutil
import unicodedata
import zipfile

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
    files = {f'{language}.combined': dictionary, f'{language}-LICENSE.txt': license_bytes,
             'encoder.pte': encoder, 'metadata.json': metadata_bytes, 'FUTO-MODEL-LICENSE.md': terms,
             # WHY standalone pinned terms: the resolver must authenticate CC terms independently of
             # an arbitrary manifest's hash of composite attribution/transform text.
             'CC-BY-SA-4.0.txt': cc}
    def asset(name, compatibility):
        data = files[name]
        result = {'path': name, 'sha256': sha(data), 'size': len(data), 'compatibility': compatibility}
        if name=='encoder.pte':
            result['metadataSha256']=sha(metadata_bytes)
        return result
    manifest = {
        'schema': 1, 'id': f'wordfreq-{language}', 'version': '3.1.1-v3-orthography1',
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
    parser.add_argument('--language', choices=ALPHABETS, required=True)
    parser.add_argument('--encoder-root', type=pathlib.Path, required=True)
    parser.add_argument('--cc-license', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--max', type=int, default=100000)
    args = parser.parse_args()
    print(f'Language {args.language}: {generate(args.wheel,args.language,args.encoder_root,args.cc_license,args.out,args.max)} dictionary bytes; quality UNVALIDATED')


if __name__ == '__main__':
    main()
