"""Private language hooks over each stock physical layout, without activating a profile."""
from pathlib import Path
import re
import shutil
import tempfile

HOOK = '''
    // WHY hook, not layout replacement: Qt retains this locale and its physical keys. The plugin
    // verifies the selected profile and its assets; unavailable swiping keeps tapping.
    sharedLayouts: ['symbols']
    function createInputMethod() {
        try {
            return Qt.createQmlObject('import QtQuick; import SwipeKeyboard; SwipeInputMethod {}', parent, "swipe-keyboard")
        } catch (e) {
            console.warn("Swipe Keyboard unavailable; using normal tapping:", e)
            return null
        }
    }
    property Item swipeCapture: null
    Component.onCompleted: {
        var c = Qt.createComponent("SwipeKeyboard", "SwipeCapture")
        if (c.status === Component.Ready)
            swipeCapture = c.createObject(keyboard.keyboardInputArea,
                { layoutItem: swipeLayout, touchArea: keyboard.keyboardInputArea })
    }
    Component.onDestruction: if (swipeCapture) swipeCapture.destroy()
'''


def patch_layout(source):
    """Preserve upstream bytes/keys; accept only an uncustomised Latin KeyboardLayout root."""
    # WHY strip only for inspection, preserving offsets: braces/keywords in quoted keys and comments
    # are not QML structure. Never rewrite the physical source while validating the hook location.
    tokens = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    code = re.sub(tokens, lambda m: re.sub(r'[^\n]', ' ', m[0]), source, flags=re.S)
    match = re.search(r'\bKeyboardLayout\s*\{', code)
    prefix = re.sub(r'(?m)^\s*import\s+[\w.]+(?:\s+\d+\.\d+)?(?:\s+as\s+\w+)?\s*;?\s*$', '',
                    code[:match.start()] if match else code)
    if not match or prefix.strip() or any(c in code for c in ('"', "'", '`')):
        raise ValueError('Unrecognised Plasma Keyboard Latin layout; stock keyboard is unchanged.')
    depth, end, root = 1, None, []
    for index in range(match.end(), len(code)):
        char = code[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
        elif depth == 1:
            root.append(char)
        if depth == 0:
            end = index
            break
    properties = ''.join(root)
    if (end is None or code[end + 1:].strip() or not re.search(r'\bSpaceKey\s*\{', code) or
            not re.search(r'\binputMode\s*:\s*InputEngine\.InputMode\.Latin\b', properties)):
        raise ValueError('Unrecognised Plasma Keyboard Latin layout; stock keyboard is unchanged.')
    if (re.search(r'\b(?:id|inputMethod)\s*:', properties) or re.search(r'\bid\s*:\s*swipeLayout\b', code) or
            any(token in code for token in ('createInputMethod', 'sharedLayouts', 'Component.onCompleted', 'Component.onDestruction'))):
        raise ValueError('Latin layout already has custom integration; refusing to replace it.')
    return source[:match.end()] + '\n    id: swipeLayout' + source[match.end():end] + HOOK + source[end:]


def patch_english(source):
    """Compatibility entry point; eligibility no longer implies an English locale."""
    return patch_layout(source)


def prepare_overrides(stock):
    """Read-only preflight shared by setup and staging; no fabricated absent locale directories."""
    stock = Path(stock)
    if not (stock / 'en_US').is_dir():
        raise ValueError('Stock Plasma Keyboard English layouts are missing.')
    targets = re.compile(r'(?:en_(?:US|GB)|(?:es|fr|de)(?:_[A-Za-z]{4})?(?:_(?:[A-Z]{2}|[0-9]{3}))?)')
    overrides = {}
    for folder in sorted(stock.iterdir()):
        if not folder.is_dir() or not targets.fullmatch(folder.name):
            continue
        main = folder / 'main.qml'
        if not main.is_file():
            marker = folder / 'main.fallback'
            # WHY empty only: this is Qt's default-fallback presence marker, NOT another locale's name.
            # Resolve it exactly as stock does; never substitute en_US or a same-language sibling.
            if not marker.is_file() or marker.read_text().strip():
                raise ValueError(f'Unsupported {folder.name} fallback; refusing to guess which layout to change.')
            main = stock / 'fallback/main.qml'
        try:
            overrides[folder.name] = patch_layout(main.read_text())
        except (OSError, ValueError) as error:
            raise ValueError(f'{folder.name}: {error}') from error
    return overrides


def build_overlay(stock, destination):
    """Stage completely before replacing our private copy; never modify upstream/user layouts."""
    stock, destination = Path(stock), Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.layouts-', dir=destination.parent) as temp:
        staged = Path(temp) / 'layouts'
        shutil.copytree(stock, staged)
        # WHY validate the staged copy: no target is published if any locale has unsupported/custom drift,
        # and a concurrent stock update cannot mismatch the source we validate and the bytes we publish.
        for locale, patched in prepare_overrides(staged).items():
            (staged / locale / 'main.fallback').unlink(missing_ok=True)
            (staged / locale / 'main.qml').write_text(patched)
        # WHY rename with rollback: a failed rebuild must not destroy the last usable layout tree.
        previous = destination.with_name(destination.name + '.previous')
        if previous.exists():
            raise ValueError('An interrupted layout replacement needs inspection: ' + str(previous))
        if destination.exists():
            destination.rename(previous)
        try:
            staged.rename(destination)
        except OSError:
            if previous.exists():
                previous.rename(destination)
            raise
        if previous.exists():
            shutil.rmtree(previous)
