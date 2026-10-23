#!/usr/bin/env python3
"""Validate an unpacked native package/DESTDIR, without starting the keyboard."""
import argparse
import json
import hashlib
import pathlib
import subprocess
import configparser
import re
import platform


def check_native_machine(library):
    # WHY ldd alone: a foreign object can fail obscurely or slip through payload
    # inspection. Every shipped executable/DSO must match the native build runner.
    expected = {'x86_64': 'Advanced Micro Devices X86-64', 'aarch64': 'AArch64'}.get(platform.machine())
    if expected is None:
        raise ValueError('Unsupported native package architecture: ' + platform.machine())
    header = subprocess.run(['readelf', '-h', str(library)], check=True, text=True, capture_output=True).stdout
    if re.findall(r'^\s*Machine:\s*(.+)$', header, re.M) != [expected]:
        raise ValueError(f'{library}: ELF architecture does not match native {platform.machine()}')


def check_public_qt_abi(library):
    # WHY final ELF, not target names: qmlcachegen can introduce private imports
    # after configuration. Undefined symbols catch versioned AND unversioned Qt internals.
    symbols = subprocess.run(['readelf', '-W', '--dyn-syms', str(library)],
                             check=True, text=True, capture_output=True).stdout
    private = [line.strip() for line in symbols.splitlines()
               if re.search(r'\bUND\b', line) and
               ('PRIVATE_API' in line or 'AOTCompiledContext' in line or 'AOTLookupValidation' in line)]
    if private:
        raise ValueError(f'{library}: unexpected private Qt ABI imports: ' + '; '.join(private))


def check_desktop(path):
    # WHY these keys: KWin discovers a virtual keyboard via the KDE protocol flag;
    # InputMethod is a kwinrc setting, not a required desktop-entry field.
    entry = configparser.ConfigParser(interpolation=None)
    entry.optionxform = str
    entry.read(path)
    try:
        keyboard = entry['Desktop Entry']
        if (keyboard.get('Type') != 'Application' or keyboard.get('Exec') != 'swipe-keyboard launch' or
                keyboard.get('X-KDE-Wayland-VirtualKeyboard') != 'true'):
            raise ValueError('Desktop entry does not expose the expected KWin keyboard launcher')
    except KeyError as error:
        raise ValueError('Desktop Entry section is missing') from error


def check_multilingual_context_assets(base, lock):
    """Pin every language's n-gram, vocabulary, contributor credit and the web/Tatoeba/notice texts.

    WHY English too: the open English n-gram replaces FUTO's neural model, so a package without it would
    silently lose English prediction quality.
    """
    models = lock.get('multilingual_context_models')
    if not isinstance(models, dict):
        raise AssertionError('Missing pinned multilingual model metadata')
    packs = base/'language-packs'
    shared = lock.get('language_files')
    assert isinstance(shared, dict), 'Missing pinned language-files release metadata'
    expected_notice = lock.get('tatoeba_license_notice_sha256')
    assert isinstance(expected_notice, str) and re.fullmatch(r'[a-f0-9]{64}', expected_notice), 'Missing pinned Tatoeba terms notice'
    assert hashlib.sha256((packs/'TATOEBA-CC-BY-2.0-FR.txt').read_bytes()).hexdigest() == expected_notice, 'Corrupt Tatoeba license notice'
    for key in ('odcTerms', 'notice'):
        entry = shared[key]
        assert hashlib.sha256((packs/entry['path']).read_bytes()).hexdigest() == entry['sha256'], f'Corrupt {entry["path"]}'
    for language in ('en', 'es', 'fr', 'de'):
        pin = models.get(language)
        assert isinstance(pin, dict), f'Missing {language} model pin'
        for name, hash_key, size_key in ((f'{language}.ngram', 'modelSha256', 'modelBytes'),
                                         (f'{language}.vocab.tsv', 'vocabSha256', 'vocabBytes'),
                                         (f'{language}-CONTRIBUTORS.txt', 'contributorsSha256', 'contributorsBytes')):
            data = (packs/name).read_bytes()
            assert len(data) == pin[size_key], f'{language} {name} size mismatch'
            assert hashlib.sha256(data).hexdigest() == pin[hash_key], f'{language} {name} hash mismatch'
        if language == 'en':
            continue  # English has no manifest; it is selected by the profile code from the same pinned files.
        manifest = json.loads((packs/f'{language}.json').read_text())
        assert manifest.get('language') == language and manifest.get('schema') == 2, f'Invalid {language} model manifest'
        assert manifest.get('contextFormat') == 'swk-ngram-v2', f'Unsupported {language} context format'
        roles = manifest.get('assets', {})
        for role, name, hash_key, size_key in (('contextModel', f'{language}.ngram', 'modelSha256', 'modelBytes'),
                                               ('contextVocab', f'{language}.vocab.tsv', 'vocabSha256', 'vocabBytes')):
            asset = roles.get(role)
            assert isinstance(asset, dict), f'{language} missing {role}'
            assert asset.get('path') == name, f'Invalid {language} {role} path'
            assert asset.get('size') == pin[size_key], f'{language} {role} size mismatch'
            assert asset.get('sha256') == pin[hash_key], f'{language} {role} hash mismatch'
            assert asset.get('compatibility') == f'{language}:context-ngram-v1', f'{language} {role} locale mismatch'
        attribution = manifest.get('contextLicense', {})
        assert attribution.get('id') == 'ODC-By-1.0 AND CC-BY-2.0-FR', f'{language} missing web/Tatoeba license metadata'
        assert attribution.get('sourceUrl') == shared['baseUrl'] + f'{language}.ngram', f'{language} source identity mismatch'
        assert attribution.get('termsPath') == 'TATOEBA-CC-BY-2.0-FR.txt'
        assert attribution.get('termsSha256') == expected_notice, f'{language} Tatoeba notice pin mismatch'
        assert attribution.get('webTermsSha256') == shared['odcTerms']['sha256'], f'{language} ODC-By terms pin mismatch'
        assert attribution.get('noticeSha256') == shared['notice']['sha256'], f'{language} language-files notice pin mismatch'
        assert attribution.get('attributionPath') == f'{language}-CONTRIBUTORS.txt', f'{language} contributor path mismatch'
        assert attribution.get('attributionSha256') == pin['contributorsSha256'], f'{language} contributor attribution mismatch'
    assert models['es'].get('locales') == ['es_ES', 'es_MX'], 'Spanish regional model sharing must be explicit'


def check_trained_models(root, lock):
    """The four neural next-word models and the swipe model must ship exactly as pinned, with their notices.

    WHY mandatory in a package: the keyboard silently runs without any model it cannot verify, so a package that
    dropped or altered one would only show up as worse suggestions. Licences: the notice (credit + sources) and the
    MIT text of FUTO's swipe data must sit in the licence folder.
    """
    models = lock.get('trained_models')
    assert isinstance(models, dict), 'Missing pinned trained-model metadata'
    base = root/'usr/share/swipe-keyboard'
    for language in ('en', 'es', 'fr', 'de'):
        pin = models['nextWord'][language]
        data = (base/'language-packs'/pin['path']).read_bytes()
        assert len(data) == pin['bytes'], f'{language} {pin["path"]} size mismatch'
        assert hashlib.sha256(data).hexdigest() == pin['sha256'], f'{language} {pin["path"]} hash mismatch'
    swipe = models['swipe']
    data = (base/'models'/pathlib.PurePosixPath(swipe['path']).name).read_bytes()
    assert len(data) == swipe['bytes'] and hashlib.sha256(data).hexdigest() == swipe['sha256'], 'swipe-model.bin differs from its pin'
    for key in ('notice', 'swipeDataTerms'):
        entry = models[key]
        data = (root/'usr/share/licenses/swipe-keyboard'/entry['path']).read_bytes()
        assert hashlib.sha256(data).hexdigest() == entry['sha256'], f'Corrupt {entry["path"]}'


def forbid_gaming_payload(root):
    """Native Arch/Fedora packages must contain no Gaming Mode file (plan Section 4: SteamOS is Flatpak-only)."""
    # WHY by file name over the whole tree: the build option, a recipe glob or a stray install() could each
    # pull the daemon, helper, units or installer in; this looks at the bytes that would actually ship.
    pattern = re.compile(r'gaming|swipekb-gamingd|swipekb-helper|swipekb-engine\.service|cef-enable-remote-debugging', re.I)
    found = sorted(str(p.relative_to(root)) for p in pathlib.Path(root).rglob('*') if pattern.search(p.name)
                   or any(pattern.search(part) for part in p.relative_to(root).parts))
    if found:
        raise ValueError('Gaming Mode files must not ship in native packages: ' + ', '.join(found))


def check(root):
    root = pathlib.Path(root)
    forbid_gaming_payload(root)
    base = root/'usr/share/swipe-keyboard'
    required = ['integration/setup.py', 'integration/layout.py', 'integration/swipekb-preflight', 'integration/swipekb-config', 'runtime.json',
                'qml/SwipeKeyboard/qmldir', 'qml/SwipeKeyboard/SwipeCapture.qml',
                'qml/SwipeKeyboard/libswipekeyboard.so', 'qml/SwipeKeyboard/libswipekeyboardplugin.so',
                'qml/SwipeKeyboard/libswipekb_engine.so',
                'models/honorable_sturgeon/model_fp32.pte', 'models/honorable_sturgeon/metadata.json',
                'models/magic_macaw/model_fp32.pte', 'models/magic_macaw/metadata.json',
                'models/hungry_jellyfish/context_lm.pte', 'models/hungry_jellyfish/vocab.txt',
                'models/hungry_jellyfish/metadata.json', 'models/FUTO-MODEL-LICENSE.md',
                'dict/en_swipe.combined']
    required += ['integration/swipekb-profilecheck']
    required += [f'language-packs/{language}.json' for language in ('es', 'fr', 'de')]
    for name in required:
        assert (base/name).is_file() and (base/name).stat().st_size, f'Missing/empty {name}'
    for name in ['usr/bin/swipe-keyboard', 'usr/share/applications/swipe-keyboard.desktop',
                 'usr/share/licenses/swipe-keyboard/LICENSE', 'usr/share/licenses/swipe-keyboard/NOTICE',
                 'usr/share/licenses/swipe-keyboard/assets.lock.json',
                 'usr/share/licenses/swipe-keyboard/TATOEBA-CC-BY-2.0-FR.txt']:
        assert (root/name).is_file(), f'Missing {name}'
    lock = json.loads((root/'usr/share/licenses/swipe-keyboard/assets.lock.json').read_text())
    for asset in lock['assets']:
        if asset['path'].startswith('models/'):
            payload = base/asset['path']
            assert hashlib.sha256(payload.read_bytes()).hexdigest() == asset['sha256'], f'Corrupt {payload}'
    assert hashlib.sha256((base/'dict/en_swipe.combined').read_bytes()).hexdigest() == lock['dictionary_sha256'], 'Dictionary differs from pinned build output'
    check_multilingual_context_assets(base, lock)
    check_trained_models(root, lock)
    assert hashlib.sha256((root/'usr/share/licenses/swipe-keyboard/TATOEBA-CC-BY-2.0-FR.txt').read_bytes()).hexdigest() == lock['tatoeba_license_notice_sha256'], 'Installed Tatoeba license notice differs from pack notice pin'
    wrapper = root/'usr/bin/swipe-keyboard'
    assert wrapper.stat().st_mode & 0o111, 'Wrapper must be executable'
    assert '/usr/share/swipe-keyboard/integration/setup.py' in wrapper.read_text(), 'Wrong runtime path'
    check_desktop(root/'usr/share/applications/swipe-keyboard.desktop')
    assert json.loads((base/'runtime.json').read_text())['qt'].startswith('6.'), 'Missing Qt ABI gate'
    for unwanted in ('eval', 'lm', 'dict-src'):
        assert not (base/unwanted).exists(), f'Unexpected optional/source payload: {unwanted}'
    helper = base/'integration/swipekb-preflight'
    assert helper.stat().st_mode & 0o111, 'Native preflight must be executable'
    config_probe = base/'integration/swipekb-config'
    assert config_probe.stat().st_mode & 0o111, 'Native KConfig probe must be executable'
    profile_probe = base/'integration/swipekb-profilecheck'
    assert profile_probe.stat().st_mode & 0o111, 'Profile diagnostic utility must be executable'
    # WHY manifest presence is not pack verification; use the same pinned resolver as runtime.
    for locale, layout in [('en-US', 'qwerty'), ('en-GB', 'qwerty'), ('es-ES', 'qwerty'),
                           ('es-MX', 'qwerty'), ('fr-FR', 'azerty'), ('de-DE', 'qwertz')]:
        query = subprocess.run([str(profile_probe), '--model-root', str(base), '--locale', locale,
                                '--layout', layout, '--mode', 'Latin'], check=True, text=True, capture_output=True)
        capability = json.loads(query.stdout)
        assert capability['swipeCapable'], f'Pack unavailable for {locale}'
        assert capability['scope'] == 'queried-installation-capability'
        if not locale.startswith('en'):
            assert capability['nextWordCapable'] and capability['contextRankingCapable'], f'Context pack unavailable for {locale}'
        if locale.startswith('es-'):
            assert capability['packId'] == 'fineweb-es', f'{locale} must use the shared Spanish pack'
    for library in [helper, config_probe, profile_probe, *(base/'qml/SwipeKeyboard').glob('*.so*')]:
        check_native_machine(library)
        check_public_qt_abi(library)
        result = subprocess.run(['ldd', str(library)], text=True, capture_output=True, check=True)
        assert 'not found' not in result.stdout, result.stdout
        resolved = result.stdout.replace(str(root.resolve()), '/STAGED')
        assert not any(x in resolved for x in ('/build/', '/home/', '/workspace/')), result.stdout
    print('Package stage validated:', root)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?')
    parser.add_argument('--elf', nargs='+', help='Check final native files for private Qt imports only')
    args = parser.parse_args()
    if args.elf:
        for library in args.elf:
            check_public_qt_abi(library)
    elif args.root:
        check(args.root)
    else:
        parser.error('root or --elf is required')
