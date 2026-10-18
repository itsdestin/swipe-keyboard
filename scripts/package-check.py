#!/usr/bin/env python3
"""Validate an unpacked native package/DESTDIR, without starting the keyboard."""
import argparse
import json
import hashlib
import pathlib
import subprocess
import configparser
import re


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


def check(root):
    root = pathlib.Path(root)
    base = root/'usr/share/swipe-keyboard'
    required = ['integration/setup.py', 'integration/layout.py', 'integration/swipekb-preflight', 'runtime.json',
                'qml/SwipeKeyboard/qmldir', 'qml/SwipeKeyboard/SwipeCapture.qml',
                'qml/SwipeKeyboard/libswipekeyboard.so', 'qml/SwipeKeyboard/libswipekeyboardplugin.so',
                'qml/SwipeKeyboard/libswipekb_engine.so',
                'models/honorable_sturgeon/model_fp32.pte', 'models/honorable_sturgeon/metadata.json',
                'models/magic_macaw/model_fp32.pte', 'models/magic_macaw/metadata.json',
                'models/hungry_jellyfish/context_lm.pte', 'models/hungry_jellyfish/vocab.txt',
                'models/hungry_jellyfish/metadata.json', 'models/FUTO-MODEL-LICENSE.md',
                'dict/en_swipe.combined']
    for name in required:
        assert (base/name).is_file() and (base/name).stat().st_size, f'Missing/empty {name}'
    for name in ['usr/bin/swipe-keyboard', 'usr/share/applications/swipe-keyboard.desktop',
                 'usr/share/licenses/swipe-keyboard/LICENSE', 'usr/share/licenses/swipe-keyboard/NOTICE',
                 'usr/share/licenses/swipe-keyboard/assets.lock.json']:
        assert (root/name).is_file(), f'Missing {name}'
    lock = json.loads((root/'usr/share/licenses/swipe-keyboard/assets.lock.json').read_text())
    for asset in lock['assets']:
        if asset['path'].startswith('models/'):
            payload = base/asset['path']
            assert hashlib.sha256(payload.read_bytes()).hexdigest() == asset['sha256'], f'Corrupt {payload}'
    assert hashlib.sha256((base/'dict/en_swipe.combined').read_bytes()).hexdigest() == lock['dictionary_sha256'], 'Dictionary differs from pinned build output'
    wrapper = root/'usr/bin/swipe-keyboard'
    assert wrapper.stat().st_mode & 0o111, 'Wrapper must be executable'
    assert '/usr/share/swipe-keyboard/integration/setup.py' in wrapper.read_text(), 'Wrong runtime path'
    check_desktop(root/'usr/share/applications/swipe-keyboard.desktop')
    assert json.loads((base/'runtime.json').read_text())['qt'].startswith('6.'), 'Missing Qt ABI gate'
    for unwanted in ('eval', 'nlm', 'lm', 'dict-src'):
        assert not (base/unwanted).exists(), f'Unexpected optional/source payload: {unwanted}'
    helper = base/'integration/swipekb-preflight'
    assert helper.stat().st_mode & 0o111, 'Native preflight must be executable'
    for library in [helper, *(base/'qml/SwipeKeyboard').glob('*.so*')]:
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
