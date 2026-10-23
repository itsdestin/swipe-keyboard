#!/usr/bin/env python3
"""M6 / M7-layer acceptance rig. Runs INSIDE the SteamOS Gaming Mode guest as the plain user (plan Section 14, M6 and M7).

Reuses the M5 rig (deploy-m6.sh puts both files in ~/m5): touchd.py running as root in the guest, the host half running
(m5_host.py up, with SWIPEKB_LOCALES listing the languages to test) and the helper switched on (m5_acceptance.py mode on).

Usage: python3 ~/m5/tools/gaming-vm/m6_acceptance.py <command> [args]
  layout-state          Steam's key grid class, the layer's view, the key names on screen
  switch NAME           press Steam's layout key until the grid says Layout_NAME (the layouts must already be active in Steam's settings)
  words NAME [N]        swipe N real words of the language of layout NAME in the search box; report top-3 and what was typed
  off NAME              a layout the engine does not serve: counting spies, stock taps, a stock swipe, no strip, nothing sent
  game-inert MODE       game-text, then taps and swipes with counting spies and a file-system diff, then closes it
  game-text MODE        a game asks for text input the way Steam's client tells its UI (MODE 0 = normal, 1 = password); watch the layer
  spacewar-up / spacewar-down   start and stop the installed Steamworks sample game (app 480, under Proton)
  game-keyboard         open Steam's keyboard over the running game and report what the layer sees
  game-close            close the keyboard and show that the page forgot everything

WHY it measures in the page and from files: pictures are for people; the checks read the DOM, the field value and the files.
Account safety: only the library search box, the editor window and made-up page elements are typed into. Enter is never sent.
Everything typed is deleted again. Nothing is bought, posted or changed except the keyboard layout list, which the operator restores.
"""
import json
import os
import sys
import time
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import m5_acceptance as m5  # noqa: E402
from m5_acceptance import ev, keys, field, strip, layer, until, touch, open_field, tap_key, clear_field, swipe, sh, SJS  # noqa: E402

GRID_JS = r"""
(function(){
 var k = document.querySelector('[data-key]'); var g = k && k.closest('[role=grid]');
 var cls = g ? String(g.getAttribute('class')||'').split(/\s+/).filter(function(c){return /^Layout_/.test(c)}) : null;
 return JSON.stringify(cls);
})()
"""
STORED_JS = "(async()=>await SteamClient.Storage.GetString('keyboard_layout'))()"

# 20 real, common words per language. Accents are typed on the path through their base key, or through their own key when Steam draws one.
WORDS = {
    'qwerty': 'world thanks please good today steam there people about would which their other first great little after should house water'.split(),
    'spanish': 'hola gracias mañana también años cómo está niño señor siempre ahora entonces después trabajo mucho casa tiempo hacer aquí día'.split(),
    'french': 'bonjour merci être déjà très français voilà comment maison toujours alors beaucoup pourquoi cette enfant école été problème où après même'.split(),
    'german': 'hallo danke schön für über größe straße wörter mädchen heute morgen wirklich schnell zusammen vielleicht freund nächste wasser mann fünf'.split(),
}


def grid_class():
    return json.loads(ev(GRID_JS))


def layout_name():
    c = grid_class()
    return c[0][len('Layout_'):] if c else None


def stored():
    return json.loads(ev(STORED_JS, title=SJS, await_promise=True))


def cmd_layout_state():
    return {'grid_class': grid_class(), 'stored_setting': stored(), 'layer': layer()['s'], 'keys': sorted(keys().keys())}


def cmd_switch(name):
    open_field()
    tries = []
    for _ in range(12):
        cur = layout_name()
        tries.append(cur)
        if cur == name:
            time.sleep(0.6)
            return {'now': cur, 'passed_through': tries, 'stored_setting': stored(), 'key_count': len(keys())}
        tap_key('SwitchKeys_Layout', 80)
        time.sleep(1.2)
    raise SystemExit('never reached %s: %s' % (name, tries))


def base_of(ch, have):
    """The key that stands for ch on this layout: its own key when Steam draws one (n-tilde, u-umlaut, sharp s), else the plain letter."""
    if ch in have:
        return ch
    d = unicodedata.normalize('NFD', ch)[0]
    return d if d in have else None


def cmd_words(name, n=20):
    open_field()
    if layout_name() != name:
        raise SystemExit('Steam shows %s, not %s: run switch first' % (layout_name(), name))
    clear_field()
    ks = keys()
    have = set(ks)
    until(lambda: strip()['strip'], 6, what='strip')
    res = []
    for w in WORDS[name][:n]:
        path = [base_of(c, have) for c in w]
        if None in path:
            res.append({'word': w, 'skipped': 'no key for %r' % [c for c, p in zip(w, path) if p is None]})
            continue
        n0 = len(field().get('value') or '')
        swipe(path, 60 * len(w) + 80)
        try:
            until(lambda: len(field().get('value') or '') > n0, 30, 0.05, 'text')
        except SystemExit:
            res.append({'word': w, 'error': 'nothing typed', 'layer': layer()['s']})
            continue
        time.sleep(0.9)
        typed = (field().get('value') or '')[n0:].strip()
        cands = [x['text'] for x in strip()['strip']['slots']]
        # screen order is 2-1-3: the strongest candidate is in the middle
        ranked = [cands[1], cands[0], cands[2]] if len(cands) == 3 else cands
        res.append({'word': w, 'typed': typed, 'top3': ranked, 'in_top3': w.lower() in [c.lower() for c in ranked], 'top1': typed.lower() == w.lower()})
        clear_field()
    ok = [r for r in res if r.get('in_top3')]
    return {'layout': name, 'swiped': len([r for r in res if 'typed' in r]), 'in_top3': len(ok), 'top1': len([r for r in res if r.get('top1')]),
            'results': res, 'layer': layer()['s']}


SPY_ON = r"""
(function(){
 var S = window.__m6spy = {stops: 0, binding: 0, orig: {}};
 S.orig.stop = Event.prototype.stopImmediatePropagation;
 Event.prototype.stopImmediatePropagation = function(){ S.stops++; return S.orig.stop.apply(this, arguments); };
 S.orig.binding = window.__swipekb;
 window.__swipekb = function(p){ S.binding++; return S.orig.binding.apply(this, arguments); };
 S.strokeBefore = window.__swipekbLayer && window.__swipekbLayer.lastStrokeId || null;
 return 'on';
})()
"""
SPY_READ = "JSON.stringify({stops: window.__m6spy.stops, binding: window.__m6spy.binding, strips: document.querySelectorAll('.swipekb-strip').length, trails: document.querySelectorAll('.swipekb-trail').length, lastStroke: window.__swipekbLayer && window.__swipekbLayer.lastStrokeId || null, strokeBefore: window.__m6spy.strokeBefore})"
SPY_OFF = "(function(){var S=window.__m6spy;if(!S)return 'none';Event.prototype.stopImmediatePropagation=S.orig.stop;window.__swipekb=S.orig.binding;delete window.__m6spy;return 'restored'})()"


def cmd_off(name):
    """A layout the engine does not serve: the layer must be completely off. Stock taps type; a stock swipe types Steam's own stray first letter."""
    open_field()
    if layout_name() != name:
        raise SystemExit('Steam shows %s, not %s' % (layout_name(), name))
    clear_field()
    time.sleep(1.0)
    out = {'layout': name, 'grid_class': grid_class(), 'layer_before': layer()['s']}
    ks = keys()
    letters = [k for k in ks if len(k) == 1 and k.isalpha()]
    out['letter_keys_on_screen'] = len(letters)
    a, b = letters[0], letters[1]
    out['strip_after_settling'] = strip()['strip'] is not None
    ev(SPY_ON)
    try:
        tap_key(a, 70); time.sleep(0.6)
        tap_key(b, 70); time.sleep(0.6)
        out['after_two_taps'] = field().get('value')
        swipe([a, letters[5], letters[9]], 300)        # a stroke across three letter keys
        time.sleep(2.0)
        out['after_swipe'] = field().get('value')
        out['spy'] = json.loads(ev(SPY_READ))
        out['layer_after'] = layer()['s']
    finally:
        out['spy_restored'] = ev(SPY_OFF)
    clear_field()
    out['field_after_clear'] = field().get('value')
    return out


# ---- the Steamworks sample game (app 480) ------------------------------------------------------------------------------
def cmd_spacewar_up():
    r = sh('steam', '-ifrunning', 'steam://rungameid/480')
    time.sleep(1)
    return {'launch_rc': r.returncode}


def cmd_spacewar_down():
    out = {}
    try:
        ev('SteamClient.Apps.TerminateApp(480, false)', title=SJS, await_promise=True)
    except Exception as e:
        out['terminate_note'] = str(e)[:100]
    time.sleep(3)
    out['processes_left'] = sh('bash', '-c', "ps -eo pid,cmd | grep -iE 'spacewar|SpaceWar' | grep -v grep | wc -l").stdout.strip()
    return out


PAGES_JS = "JSON.stringify(Array.from(document.querySelectorAll('input,textarea')).map(function(e){var r=e.getBoundingClientRect();return {tag:e.tagName,type:e.type,focused:e===document.activeElement,w:Math.round(r.width),h:Math.round(r.height)}}))"


def pages():
    from swipekb_helper import cdp
    return [(t.get('title'), t.get('type')) for t in cdp.http_json(m5.PORT, '/json/list')]


def cmd_game_text(mode):
    """WHY this way: the real Steamworks call (a game asking for text input) reaches Steam's UI as a 'game keyboard message'; the hidden
    page hands it to the same function the client's message would call. No Steamworks sample is available here, so the rig makes the call
    itself, with exactly the fields Steam's UI reads (m_eInputMode: 0 = normal, 1 = password). Test rig only: the product never does this."""
    mode = int(mode)
    out = {'mode': mode, 'pages_before': pages()}
    msg = {'nAppID': 480, 'm_dwOverlayPID': 0, 'm_dwPID': 0, 'm_hPipe': 0, 'm_bOpen': True, 'm_pchDescription': 'M6 rig prompt', 'm_pchExistingText': '',
           'm_unCharMax': 32, 'm_bSubmitted': False, 'm_eInputMode': mode, 'm_unSubmittedText': 0, 'm_eLineInputMode': 0}
    ev('SteamUIStore.OnGameKeyboardMessage(%s)' % json.dumps(msg), title=SJS)
    try:
        until(lambda: len(keys()) >= 24 or json.loads(ev(PAGES_JS)), 10, 0.3, 'the game text dialog')
    except SystemExit as e:
        out['error'] = str(e)
    time.sleep(2.5)
    out['inputs_in_page'] = json.loads(ev(PAGES_JS))
    out['field_seen_by_layer'] = field()
    out['keys_on_screen'] = len(keys())
    out['layer'] = layer()['s']
    out['strip_shown'] = bool(strip()['strip'])
    return out


def cmd_game_inert(mode):
    """A game-requested text box. Password kind (1): the layer must be completely off. Normal kind (0): swipes work there (plain mode).
    Only made-up letters are typed, and they are deleted again."""
    mode = int(mode)
    out = cmd_game_text(mode)
    f = field()
    out['field_type'] = f.get('type')
    clear_field_game()
    before = m5.fs_listing()
    ev(SPY_ON)
    try:
        for ch in 'abc':
            tap_key(ch, 70); time.sleep(0.4)
        out['value_after_three_taps'] = field().get('value')
        swipe(list('world'), 300)
        time.sleep(2.5)
        out['value_after_swipe'] = field().get('value')
        swipe(['q', 'p'], 250)
        time.sleep(2.5)
        out['value_after_second_swipe'] = field().get('value')
        out['spy'] = json.loads(ev(SPY_READ))
        out['layer_after'] = layer()['s']
        out['strip_shown_after'] = bool(strip()['strip'])
    finally:
        out['spy_restored'] = ev(SPY_OFF)
    clear_field_game()
    out['value_after_clear'] = field().get('value')
    time.sleep(1.0)
    after = m5.fs_listing()
    out['fs_files_compared'] = len(before)
    out['fs_identical'] = before == after
    out['fs_differences'] = {k: [before.get(k), after.get(k)] for k in set(before) | set(after) if before.get(k) != after.get(k)}
    out['closed'] = cmd_game_text_end()
    return out


def clear_field_game():
    for _ in range(40):
        if not (field().get('value') or ''):
            return
        tap_key('Backspace', 90)
        time.sleep(0.25)


def focus_name():
    return sh('bash', '-c', 'DISPLAY=:1 xdotool getwindowfocus getwindowname').stdout.strip()


def cmd_game_keyboard():
    """Steam's keyboard opened over the running game (the same call as the Steam+X chord's keyboard: steam://open/keyboard)."""
    out = {'focus_before': focus_name(), 'layer_before': layer()['s']}
    t0 = time.time()
    sh('steam', '-ifrunning', 'steam://open/keyboard')
    until(lambda: len(keys()) >= 24, 20, 0.2, 'keyboard over the game')
    out['seconds_to_keys'] = round(time.time() - t0, 2)
    until(lambda: strip()['strip'], 8, what='strip over the game')
    time.sleep(1.0)
    out['field_seen_by_layer'] = field()
    out['layer_open'] = layer()['s']
    out['focus_with_keyboard_open'] = focus_name()
    out['layout'] = layout_name()
    # stock taps, then two swipes; the helper's own count says whether the text calls went through (what the game does with them is
    # shown by the Proton editor test, which has a text box to read)
    for ch in 'we':
        tap_key(ch, 70); time.sleep(0.4)
    out['memory_after_taps'] = layer()['s']['bufferLength']
    for w in ('world', 'thanks'):
        n0 = layer()['s']['bufferLength']
        swipe(list(w), 60 * len(w))
        until(lambda: layer()['s']['bufferLength'] > n0, 30, 0.05, 'word typed')
        time.sleep(0.8)
    out['layer_after_swipes'] = layer()['s']
    out['candidates'] = [x['text'] for x in strip()['strip']['slots']]
    out['focus_after'] = focus_name()
    return out


def cmd_game_close():
    out = {'before': layer()['s']}
    sh('steam', '-ifrunning', 'steam://close/keyboard')
    t0 = time.time()
    try:
        until(lambda: layer()['s']['bufferLength'] == 0 and not layer()['s']['stripShown'] and len(keys()) == 0, 10, 0.05, 'memory wiped')
    except SystemExit as e:
        out['error'] = str(e)
    out['seconds_to_forget'] = round(time.time() - t0, 3)
    out['after'] = layer()['s']
    out['focus_after_close'] = focus_name()
    return out


# ---- a Windows text editor under Proton, added as a non-Steam shortcut: the closest stand-in for a game with a text box ------------------
HOME = Path.home()
PROTON = HOME / '.local/share/Steam/steamapps/common/Proton - Experimental'
NOTEPAD_EXE = PROTON / 'files/lib/wine/x86_64-windows/notepad.exe'        # Wine's editor, shipped inside Valve's Proton (not downloaded)
P_SHORTCUT = 'SwipeM6ProtonEditor'
P_APPID_FILE = HOME / 'm5' / 'proton_appid.txt'
P_OUT = HOME / 'm5' / 'proton-note.txt'


def xdo(*a):
    return sh('bash', '-c', 'DISPLAY=:1 xdotool ' + ' '.join(a))


def cmd_proton_up():
    js = 'SteamClient.Apps.AddShortcut(%s, %s, %s, "")' % (json.dumps(P_SHORTCUT), json.dumps(str(NOTEPAD_EXE)), json.dumps(str(PROTON)))
    appid = ev(js, title=SJS, await_promise=True)
    P_APPID_FILE.write_text(str(appid))                 # written FIRST so cleanup can always find it
    out = {'appid': appid}
    out['specify_compat'] = ev('SteamClient.Apps.SpecifyCompatTool(%d, "proton_experimental")' % int(appid), title=SJS, await_promise=True)
    gid = (int(appid) << 32) | 0x02000000
    r = sh('steam', '-ifrunning', 'steam://rungameid/%d' % gid)
    out['launch_rc'] = r.returncode
    until(lambda: sh('bash', '-c', "pgrep -fa 'notepad.exe' | grep -v pgrep | head -1").stdout.strip(), 120, 1.0, 'the editor process')
    time.sleep(8)
    out['focus'] = focus_name()
    return out


def cmd_proton_run():
    out = {'focus_before': focus_name()}
    sh('steam', '-ifrunning', 'steam://open/keyboard')
    until(lambda: len(keys()) >= 24, 20, 0.5, 'keyboard over the editor')
    until(lambda: strip()['strip'], 8, what='strip over the editor')
    time.sleep(1.0)
    out['field_seen_by_layer'] = field()
    for ch in 'we':
        tap_key(ch, 70); time.sleep(0.5)
    tap_key(' ', 70); time.sleep(0.6)
    words = []
    for w in ('world', 'thanks', 'good'):
        n0 = layer()['s']['bufferLength']
        swipe(list(w), 60 * len(w))
        until(lambda: layer()['s']['bufferLength'] > n0, 30, 0.05, 'word typed')
        time.sleep(1.0)
        words.append({'swiped': w, 'candidates_left_to_right': [x['text'] for x in strip()['strip']['slots']]})
    out['words'] = words
    out['focus_with_keyboard'] = focus_name()
    return out


def cmd_proton_save():
    """Close the keyboard, then Ctrl+S in the editor and name the file, so the typed text can be read back from disk."""
    sh('steam', '-ifrunning', 'steam://close/keyboard')
    time.sleep(2)
    P_OUT.unlink(missing_ok=True)
    xdo('key', 'ctrl+s'); time.sleep(2.5)
    xdo('type', '--delay', '60', json.dumps('Z:' + str(P_OUT).replace('/', '\\'))); time.sleep(0.5)
    xdo('key', 'Return'); time.sleep(2.5)
    data = P_OUT.read_bytes() if P_OUT.exists() else None
    return {'file_exists': data is not None, 'bytes_hex': data.hex() if data is not None else None, 'text_repr': repr(data.decode('latin-1')) if data is not None else None,
            'focus': focus_name()}


def cmd_proton_down():
    out = {}
    appid = int(P_APPID_FILE.read_text()) if P_APPID_FILE.exists() else None
    out['appid'] = appid
    if appid is not None:
        try:
            ev('SteamClient.Apps.TerminateApp(%d, false)' % appid, title=SJS, await_promise=True)
        except Exception as e:
            out['terminate_note'] = str(e)[:100]
        time.sleep(4)
        for pat in ('notepad.exe',):
            left = sh('bash', '-c', "pgrep -f %s | head" % pat).stdout.split()
            out['left_' + pat] = left
        try:
            ev('SteamClient.Apps.SpecifyCompatTool(%d, "")' % appid, title=SJS, await_promise=True)
        except Exception as e:
            out['compat_clear_note'] = str(e)[:100]
        ev('SteamClient.Apps.RemoveShortcut(%d)' % appid, title=SJS, await_promise=True)
        time.sleep(2.0)
    vdfs = list((HOME / '.local/share/Steam/userdata').glob('*/config/shortcuts.vdf'))
    out['shortcuts_vdf_mentions'] = sum(p.read_bytes().count(P_SHORTCUT.encode()) for p in vdfs)
    P_OUT.unlink(missing_ok=True)
    if out['shortcuts_vdf_mentions'] == 0:
        P_APPID_FILE.unlink(missing_ok=True)
    return out


def cmd_game_text_end():
    ev('SteamUIStore.OnGameKeyboardMessage(%s)' % json.dumps({'nAppID': 480, 'm_dwOverlayPID': 0, 'm_bOpen': False}), title=SJS)
    time.sleep(2)
    return {'keys_on_screen': len(keys()), 'layer': layer()['s']}


def main():
    c, a = sys.argv[1], sys.argv[2:]
    out = {'layout-state': cmd_layout_state, 'switch': lambda: cmd_switch(a[0]), 'words': lambda: cmd_words(a[0], int(a[1]) if len(a) > 1 else 20),
           'off': lambda: cmd_off(a[0]), 'game-text': lambda: cmd_game_text(a[0]), 'game-text-end': cmd_game_text_end, 'proton-up': cmd_proton_up, 'proton-run': cmd_proton_run, 'proton-save': cmd_proton_save, 'proton-down': cmd_proton_down, 'game-keyboard': cmd_game_keyboard, 'game-close': cmd_game_close, 'game-inert': lambda: cmd_game_inert(a[0]), 'spacewar-up': cmd_spacewar_up, 'spacewar-down': cmd_spacewar_down}[c]()
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
