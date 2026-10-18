"""Private stock-layout overlay: English swipe hook, no theme or tablet dependencies."""
from pathlib import Path
import re
import shutil
import tempfile

HOOK = '''
    // WHY English-only: the bundled dictionary/models are English. Keep every other locale stock.
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


def patch_english(source):
    """Preserve all upstream keys; reject unknown formats instead of partially patching."""
    match = re.search(r'\bKeyboardLayout\s*\{', source)
    if (not match or 'SpaceKey' not in source or
            'inputMode: InputEngine.InputMode.Latin' not in source or
            not source.rstrip().endswith('}')):
        raise ValueError('Unrecognised Plasma Keyboard English layout; stock keyboard is unchanged.')
    body = source[match.end():]
    root_properties = re.split(r'\b\w+\s*\{', body, maxsplit=1)[0]
    if (re.search(r'\bid\s*:', root_properties) or
            any(token in source for token in ('createInputMethod', 'sharedLayouts', 'Component.onCompleted', 'Component.onDestruction'))):
        raise ValueError('English layout already has custom integration; refusing to replace it.')
    end = source.rfind('}')
    return source[:match.end()] + '\n    id: swipeLayout' + source[match.end():end] + HOOK + source[end:]


def build_overlay(stock, destination):
    """Stage completely before replacing our private copy; never modify upstream/user layouts."""
    stock, destination = Path(stock), Path(destination)
    english = stock / 'en_US/main.qml'
    if not english.is_file():
        fallback = stock / 'en_US/main.fallback'
        # WHY empty: Qt's .fallback is a presence marker, not a file naming a destination.
        if not fallback.is_file() or fallback.read_text().strip():
            raise ValueError('Unsupported en_US fallback; refusing to guess which layout to change.')
        english = stock / 'fallback/main.qml'
    patched = patch_english(english.read_text())
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.layouts-', dir=destination.parent) as temp:
        staged = Path(temp) / 'layouts'
        shutil.copytree(stock, staged)
        (staged / 'en_US/main.fallback').unlink(missing_ok=True)
        (staged / 'en_US/main.qml').write_text(patched)
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
