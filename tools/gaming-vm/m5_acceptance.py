#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""M5 acceptance rig. Runs INSIDE the SteamOS Gaming Mode guest as the plain user (plan Section 14, M5).

Deploy with deploy-m5.sh. Needs: touchd.py running as root in the guest (socket /tmp/touchd.sock), the guest's Steam signed in with the
debug port open, and the host half running (m5_host.py up) so the engine's socket is forwarded to ~/m5/run/engine.sock.

Usage: python3 ~/m5/tools/gaming-vm/m5_acceptance.py <command> [args]
  mode on|off         helper running (pointed at the forwarded engine) with the layer + session in the page, or stopped and removed
  hello               swipe "hello" in the search box: field value, timing, strip contents/position, tap a candidate, replacement
  scales              the strip position at several page sizes (Steam's UI scale stand-ins), by DOM measurement
  private             the Private chip: off, stock behaviour, on again
  password            a password box as the layer sees it: counting spies, no strip, no binding calls, nothing stopped
  kwrite-up           add the temporary non-Steam shortcut and launch the editor (cleaned up by kwrite-down, which is MANDATORY)
  kwrite-run          open Steam's keyboard over the editor, type and swipe there (unknown mode)
  kwrite-close        close the keyboard and show that the page forgot everything
  kwrite-down         close the editor, remove the shortcut through Steam's own API, verify it is gone
  fs snap|cmp NAME..  listing of every folder the guest-side apps can write (with checksums) / comparison
  layer               what the page layer reports

WHY it measures in the page and from files: pictures are for people; the checks read the DOM, the field value and the files.
Account safety: only the library search box, the editor window and a made-up page element are typed into. Enter is never sent.
Everything typed is deleted again. Nothing is bought, posted or changed in any setting.
"""
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

HOME = Path.home()
sys.path.insert(0, str(HOME / 'm5' / 'gaming' / 'helper'))
from swipekb_helper import cdp, ws  # noqa: E402

PORT = 8080
BP, SJS = 'Steam Big Picture Mode', 'SharedJSContext'
UNIT = 'swipekb-m5-helper'
STATUS = HOME / '.local' / 'state' / 'swipekb-gaming' / 'status.json'
RUN = HOME / 'm5' / 'run'
NOTE = HOME / 'm5' / 'note.txt'
SHORTCUT = 'SwipeM5KWrite'
APPID_FILE = HOME / 'm5' / 'appid.txt'

_c = {}


def sh(*a, **k):
    return subprocess.run(a, capture_output=True, text=True, **k)


def target(title):
    for t in cdp.http_json(PORT, '/json/list'):
        if t.get('type') == 'page' and t.get('title') == title:
            return t['id']
    raise SystemExit('no page titled %s' % title)


def ev(js, title=BP, timeout=8.0, await_promise=False):
    for attempt in (0, 1):
        try:
            if title not in _c:
                _c[title] = cdp.Cdp.open(PORT, cdp.page_path(target(title)), timeout=timeout)
            if await_promise:
                r = _c[title].call('Runtime.evaluate', {'expression': js, 'returnByValue': True, 'awaitPromise': True}, timeout)
                if 'exceptionDetails' in r:
                    raise cdp.CdpError(str(r['exceptionDetails'])[:300])
                return r.get('result', {}).get('value')
            return _c[title].evaluate(js, timeout=timeout)
        except (ws.WsError, OSError, cdp.CdpError):
            _c.pop(title, None)
            if attempt:
                raise


def touch(*lines):
    s = socket.socket(socket.AF_UNIX)
    s.connect('/tmp/touchd.sock')
    f = s.makefile('rw')
    for l in lines:
        f.write(l + '\n')
        f.flush()
        r = f.readline().strip()
        if r != 'ok':
            raise SystemExit('touchd: ' + r)
    s.close()


KEYS_JS = r"""
(function(){
 var out = {}, nodes = document.querySelectorAll('[data-key]'), k = 1280 / window.innerWidth;
 for (var i = 0; i < nodes.length; i++) {
   var n = nodes[i];
   if (n.hasAttribute('data-key-is-literal')) continue;
   if (n.parentElement && n.parentElement.closest('[data-key]')) continue;
   var r = n.getBoundingClientRect();
   out[n.getAttribute('data-key')] = [Math.round((r.left + r.width/2)*k), Math.round((r.top + r.height/2)*k), Math.round(r.width*k), Math.round(r.height*k)];
 }
 return JSON.stringify(out);
})()
"""
FIELD_JS = "(function(){var a=document.activeElement;return JSON.stringify({tag:a&&a.tagName,type:a&&a.type,value:a&&a.value})})()"
STRIP_JS = r"""
(function(){
 var s = document.querySelector('.swipekb-strip'), k = 1280 / window.innerWidth;
 var keys = document.querySelectorAll('[data-key]'), top = 1e9, bottom = -1e9, left = 1e9, right = -1e9, rects = [];
 for (var i = 0; i < keys.length; i++) {
   if (keys[i].hasAttribute('data-key-is-literal')) continue;
   var r = keys[i].getBoundingClientRect(); if (!r.width) continue;
   top = Math.min(top, r.top); bottom = Math.max(bottom, r.bottom); left = Math.min(left, r.left); right = Math.max(right, r.right);
   rects.push([r.left, r.top, r.right, r.bottom]);
 }
 var out = {keys_top: top, keys_left: left, keys_right: right, inner: [window.innerWidth, window.innerHeight], strip: null};
 if (s) {
   var q = s.getBoundingClientRect();
   var overlap = 0;
   for (var j = 0; j < rects.length; j++) { var c = rects[j]; if (q.left < c[2] && q.right > c[0] && q.top < c[3] && q.bottom > c[1]) overlap++; }
   var slots = s.querySelectorAll('[data-swipekb^=slot]'), cs = [];
   for (var m = 0; m < slots.length; m++) { var b = slots[m].getBoundingClientRect(); cs.push({text: slots[m].textContent, x: Math.round((b.left+b.width/2)*k), y: Math.round((b.top+b.height/2)*k)}); }
   var chip = s.querySelector('[data-swipekb=private]'), cb = chip.getBoundingClientRect();
   out.strip = {left: q.left, top: q.top, right: q.right, bottom: q.bottom, gap_to_keys: top - q.bottom, overlaps_keys: overlap,
                slots: cs, chip: {text: chip.textContent, x: Math.round((cb.left+cb.width/2)*k), y: Math.round((cb.top+cb.height/2)*k)}};
 }
 return JSON.stringify(out);
})()
"""
LAYER_JS = ("JSON.stringify(window.__swipekbLayer ? {v: window.__swipekbLayer.version, g: window.__swipekbLayer.gestures ? "
            "window.__swipekbLayer.gestures.state() : null, s: window.__swipekbLayer.session ? window.__swipekbLayer.session.debug() : null,"
            " serr: window.__swipekbLayer.sessionError || null, attr: document.documentElement.getAttribute('data-swipekb-gestures')} : null)")


def keys():
    return json.loads(ev(KEYS_JS))


def field():
    return json.loads(ev(FIELD_JS))


def strip():
    return json.loads(ev(STRIP_JS))


def layer():
    return json.loads(ev(LAYER_JS))


def until(cond, limit=15.0, step=0.1, what='condition'):
    end = time.time() + limit
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    raise SystemExit('timed out waiting for ' + what)


# ---- helper / layer ----------------------------------------------------------------------------------------
def unit_active():
    return sh('systemctl', '--user', 'is-active', UNIT).stdout.strip() == 'active'


def mode(which):
    if which == 'off':
        sh('systemctl', '--user', 'stop', UNIT)
        until(lambda: layer() is None, 10, what='layer gone after helper stop')
        return {'mode': 'off'}
    if not unit_active():
        sh('systemctl', '--user', 'reset-failed', UNIT)
        r = sh('systemd-run', '--user', '--unit=' + UNIT, '--collect', '--setenv=PYTHONPATH=%s' % (HOME / 'm5' / 'gaming' / 'helper'),
               '--setenv=PYTHONDONTWRITEBYTECODE=1', '--setenv=SWIPEKB_ENGINE_SOCKET=%s' % (RUN / 'engine.sock'),
               '--setenv=SWIPEKB_ENGINE_TOKEN=%s' % (RUN / 'engine.token'), '-p', 'Restart=always', '-p', 'RestartSec=3', 'python3', '-m', 'swipekb_helper')
        if r.returncode:
            raise SystemExit(r.stderr)
    l = until(lambda: (layer() or {}).get('g') == 'on' and (layer() or {}).get('s') and layer(), 20, what='layer with session')
    return {'mode': 'on', 'layer': l, 'status': json.loads(STATUS.read_text()) if STATUS.exists() else None}


def open_field():
    """Touch the library search box (a local filter; nothing is sent anywhere) so Steam opens its keyboard on it.
    WHY the box is looked up in the page: the top bar's icons move between Steam builds and notifications (M4 tapped a fixed spot)."""
    f = field()
    if f.get('type') == 'search' and len(keys()) >= 24:
        return f
    box = json.loads(ev("(function(){var e=document.querySelector('input[type=search]');if(!e)return 'null';var r=e.getBoundingClientRect(),k=1280/window.innerWidth;"
                        "return JSON.stringify([Math.round((r.left+r.width/2)*k),Math.round((r.top+r.height/2)*k),e.placeholder])})()"))
    if not box or 'Search' not in box[2]:
        raise SystemExit('the search box is not where expected: %s' % (box,))
    touch('tap %d %d 80' % (box[0], box[1]))
    time.sleep(2.0)
    f = field()
    if f.get('type') != 'search' or len(keys()) < 24:
        raise SystemExit('could not open a search field with the keyboard: %s' % f)
    return f


def tap_key(name, hold=70):
    k = keys()[name]
    touch('tap %d %d %d' % (k[0], k[1], hold))


def clear_field():
    for _ in range(6):
        left = len(field().get('value') or '')
        if not left:
            return True
        for _i in range(min(left, 60)):
            tap_key('Backspace', 90)
            time.sleep(0.25)
        time.sleep(1.0)
    return not field().get('value')


def swipe(word_keys, ms=350):
    ks = keys()
    pts = ' '.join('%d,%d' % (ks[c][0], ks[c][1]) for c in word_keys)
    touch('drag %d %s' % (ms, pts))


# ---- commands ----------------------------------------------------------------------------------------------
CANARY = 'abcdefgh '


def text_call_healthy(tries=8, gap=4.0):
    """Does Steam's own text call type what it is given? WHY (found in the SteamOS VM, M8): independent of our layer and helper
    (reproduced with both stopped), Steam's ControllerKeyboardSendText sometimes types garbage in a guest that is slow or busy:
    the first letters are right, the rest come out as counting digits (`abcdefgh ` -> `ab3456789`, `\\b` -> `0`), and the call can lag
    by several seconds. A swipe test run then would blame our code for Steam's behaviour, so every rig step that types waits for
    a clean canary first. The canary is cleared with taps on Backspace, never with the text call that may be the broken part."""
    for _ in range(tries):
        ev("SteamClient.Input.ControllerKeyboardSendText(%s)" % json.dumps(CANARY), title='SharedJSContext')
        time.sleep(3.0)
        v = field().get('value')
        clear_field()
        if v == CANARY:
            return True
        time.sleep(gap)
    raise SystemExit("Steam's own text call keeps typing garbage in this guest (last canary read back %r): not a Swipe Keyboard fault; wait or restart the guest" % v)


def cmd_hello():
    """Swipe words in the search box. 'world' is the clean case (the engine's top word is the typed word). 'hello' is the finding case:
    on a straight-line scripted path the engine ranks 'help' above 'hello' (same as the desktop engine; see the M5 status), so the
    word is then fixed by tapping the 'hello' candidate, which exercises the replacement path."""
    open_field(); clear_field(); text_call_healthy()
    out = {'layer': layer()}
    until(lambda: strip()['strip'], 5, what='strip shown')
    out['strip_before_swipe'] = strip()
    runs = []
    for word, how in (('world', 'swipe'), ('world', 'swipe again (warm)'), ('hello', 'swipe, then tap the candidate that says hello')):
        t0 = time.time()
        swipe(list(word), 60 * len(word))
        v = until(lambda: field().get('value'), 40, 0.05, 'text from the swipe')
        run = {'word_swiped': word, 'how': how, 'seconds_from_touch_start_to_text': round(time.time() - t0, 3), 'field_value': v}
        time.sleep(1.2)
        s = strip()
        run['strip'] = s['strip']
        run['candidates_left_to_right'] = [x['text'] for x in s['strip']['slots']]
        if word == 'hello':
            slot = next(x for x in s['strip']['slots'] if x['text'] == 'hello')
            before = field()['value']
            touch('tap %d %d 80' % (slot['x'], slot['y']))
            until(lambda: field()['value'] != before, 10, 0.05, 'replacement')
            time.sleep(0.8)
            run['after_tapping_hello_slot'] = {'field_value': field()['value'], 'keyboard_still_open': len(keys()) >= 24, 'field_type': field().get('type')}
            out['strip_after_pick'] = strip()['strip']
        runs.append(run)
        clear_field()
    out['runs'] = runs
    out['field_after_clear'] = field()['value']
    return out


def cmd_scales():
    open_field(); until(lambda: strip()['strip'], 5, what='strip')
    res = []
    sizes = [(None, None, None), (1600, 1000, 0.8), (1066, 666, 1.2), (1920, 1200, 1280 / 1920)]
    c = cdp.Cdp.open(PORT, cdp.page_path(target(BP)), timeout=8)
    try:
        for w, h, dsf in sizes:
            if w:
                c.call('Emulation.setDeviceMetricsOverride', {'width': w, 'height': h, 'deviceScaleFactor': dsf, 'mobile': False})
            else:
                c.call('Emulation.clearDeviceMetricsOverride')
            time.sleep(1.5)
            s = None
            for _ in range(20):
                s = strip()
                if s['strip'] and len(keys()) >= 24:
                    break
                time.sleep(0.25)
            st = s['strip']
            res.append({'page_size': s['inner'], 'keys_top': round(s['keys_top'], 1), 'strip_top': st and round(st['top'], 1),
                        'strip_bottom': st and round(st['bottom'], 1), 'gap_to_top_row': st and round(st['gap_to_keys'], 1),
                        'overlaps_any_key': st and st['overlaps_keys'], 'inside_keyboard_width': st and (st['left'] >= s['keys_left'] - 1 and st['right'] <= s['keys_right'] + 1),
                        'strip_height': st and round(st['bottom'] - st['top'], 1)})
    finally:
        c.call('Emulation.clearDeviceMetricsOverride')
        c.close()
    time.sleep(1.0)
    return {'results': res, 'restored': strip()['inner']}


def cmd_private():
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    out = {}
    s = strip()['strip']
    chip = s['chip']
    touch('tap %d %d 70' % (chip['x'], chip['y']))
    time.sleep(0.8)
    out['after_chip_tap'] = {'layer': layer()['s'], 'chip_text': strip()['strip']['chip']['text'], 'keyboard_open': len(keys()) >= 24, 'field_type': field().get('type')}
    # swipes are now stock Steam: the arbiter steps aside, so the first letter of the stroke is typed
    swipe(['q', 'p'], 200)
    time.sleep(1.5)
    out['value_after_swipe_while_private'] = field()['value']
    clear_field()
    chip = strip()['strip']['chip']
    touch('tap %d %d 70' % (chip['x'], chip['y']))
    time.sleep(0.8)
    out['after_second_chip_tap'] = {'layer': layer()['s'], 'chip_text': strip()['strip']['chip']['text']}
    swipe(list('hello'))
    out['value_after_swipe_when_back_on'] = until(lambda: field().get('value'), 20, 0.1, 'text after turning back on')
    time.sleep(1.0)
    clear_field()
    out['field_after_clear'] = field()['value']
    return out


SPY_ON = r"""
(function(){
 var S = window.__m5spy = {stops: 0, binding: 0, orig: {}};
 S.orig.stop = Event.prototype.stopImmediatePropagation;
 Event.prototype.stopImmediatePropagation = function(){ S.stops++; return S.orig.stop.apply(this, arguments); };
 S.orig.binding = window.__swipekb;
 window.__swipekb = function(p){ S.binding++; return S.orig.binding.apply(this, arguments); };
 // The layer decides "password" from document.activeElement.type. Steam keeps its real focus untouched; only the layer's view changes.
 var fake = document.createElement('input'); fake.type = 'password';
 S.realDesc = Object.getOwnPropertyDescriptor(Document.prototype, 'activeElement');
 Object.defineProperty(document, 'activeElement', {get: function(){ return fake; }, configurable: true});
 S.nodes0 = document.querySelectorAll('.swipekb-strip,.swipekb-trail').length;
 return 'on';
})()
"""
SPY_OFF = r"""
(function(){
 var S = window.__m5spy; if (!S) return 'none';
 delete document.activeElement;
 Event.prototype.stopImmediatePropagation = S.orig.stop;
 window.__swipekb = S.orig.binding;
 var r = {stops: S.stops, binding: S.binding};
 delete window.__m5spy; return JSON.stringify(r);
})()
"""
SPY_READ = ("JSON.stringify({stops: window.__m5spy.stops, binding: window.__m5spy.binding, strips: document.querySelectorAll('.swipekb-strip').length, "
            "trails: document.querySelectorAll('.swipekb-trail').length, g: window.__swipekbLayer.gestures.stats(), s: window.__swipekbLayer.session.debug(), "
            "lastStroke: (window.__swipekbLayer.lastStrokeId || null)})")
# What reaches Steam's own listeners (bubble phase, after our capture-phase handler): if the layer swallows a touch, these stay at zero.
REACH_ON = r"""
(function(){
 var R = window.__m5reach = {down: 0, move: 0, up: 0, fns: []};
 [['pointerdown','down'],['pointermove','move'],['pointerup','up']].forEach(function(p){
   var f = function(e){ if (e.isTrusted) R[p[1]]++; }; R.fns.push([p[0], f]); window.addEventListener(p[0], f, false); });
 return 'on';
})()
"""
REACH_RESET = "(function(){var R=window.__m5reach;R.down=R.move=R.up=0;return 1})()"
REACH_READ = "JSON.stringify({down: window.__m5reach.down, move: window.__m5reach.move, up: window.__m5reach.up})"
REACH_OFF = "(function(){var R=window.__m5reach;if(!R)return 'none';R.fns.forEach(function(p){window.removeEventListener(p[0],p[1],false)});delete window.__m5reach;return 'off'})()"
GESTURE_STATS = "JSON.stringify(window.__swipekbLayer.gestures.stats())"


def watched_dirs():
    cands = [HOME / '.local/state/swipekb-gaming', HOME / '.config/swipe-keyboard', HOME / '.local/share/swipe-keyboard-gaming',
             HOME / '.local/share/swipe-keyboard', HOME / '.cache/swipe-keyboard', HOME / '.var/app/org.itsdestin.SwipeKeyboard',
             HOME / '.var/app/org.itsdestin.SwipeKeyboard.GamingHelper', RUN]
    return [d for d in cands if d.exists()]


def fs_listing():
    out = {}
    for d in watched_dirs():
        for p in sorted([d] + list(d.rglob('*'))):
            try:
                if p.is_socket():
                    out[str(p)] = ['socket']
                elif p.is_dir():
                    out[str(p)] = ['dir']
                elif p.is_file():
                    out[str(p)] = [p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest()]
            except OSError:
                pass
    return out


def cmd_password():
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    out = {'strip_before_password': bool(strip()['strip'])}
    ev(REACH_ON)
    try:
        # CONTROL: the same swipe in a plain box. The layer swallows it, so nothing reaches Steam's own listeners and a binding call is made.
        ev(REACH_RESET)
        swipe(['q', 'p'], 250); time.sleep(1.5)
        out['control_plain_box_swipe_reached_steam'] = json.loads(ev(REACH_READ))
        clear_field()
        before = fs_listing()
        g0 = json.loads(ev(GESTURE_STATS))
        out['stroke_id_before'] = ev("(window.__swipekbLayer.lastStrokeId || null)")
        ev(SPY_ON)
        ev(REACH_RESET)
        try:
            time.sleep(1.0)                  # the session's tick must notice the "password box"
            out['strip_while_password'] = strip()['strip']
            for ch in 'abc':
                tap_key(ch, 70); time.sleep(0.3)
            swipe(list('hello'), 300)        # a swipe in a password box is stock Steam's business
            swipe(['q', 'p'], 250)
            time.sleep(2.5)
            out['spy'] = json.loads(ev(SPY_READ))
            out['reached_steam_while_password'] = json.loads(ev(REACH_READ))
        finally:
            out['spy_restored'] = ev(SPY_OFF)
        g1 = json.loads(ev(GESTURE_STATS))
        out['gesture_counters_changed'] = {k: g1[k] - g0[k] for k in g0 if g1[k] != g0[k]}
        out['stroke_id_after'] = out['spy']['lastStroke']
    finally:
        ev(REACH_OFF)
    clear_field()
    time.sleep(1.0)
    after = fs_listing()
    out['strip_back_after_password'] = bool(strip()['strip'])
    out['fs_files_compared'] = len(before)
    out['fs_identical'] = before == after
    out['fs_differences'] = {k: [before.get(k), after.get(k)] for k in set(before) | set(after) if before.get(k) != after.get(k)}
    return out


# ---- the non-Steam shortcut route (unknown mode) ------------------------------------------------------------------------
def cmd_kwrite_up():
    NOTE.write_text('')
    js = 'SteamClient.Apps.AddShortcut(%s, "/usr/bin/kwrite", "/home/deck", %s)' % (json.dumps(SHORTCUT), json.dumps(str(NOTE)))
    appid = ev(js, title=SJS, await_promise=True)
    APPID_FILE.write_text(str(appid))            # written FIRST so cleanup can always find it
    gid = (int(appid) << 32) | 0x02000000
    r = sh('steam', '-ifrunning', 'steam://rungameid/%d' % gid)
    until(lambda: sh('pgrep', '-x', 'kwrite').stdout.strip(), 40, 0.5, 'kwrite running')
    time.sleep(3.0)
    return {'appid': appid, 'launch_rc': r.returncode, 'kwrite_pid': sh('pgrep', '-x', 'kwrite').stdout.split()}


def cmd_kwrite_run():
    out = {}
    sh('steam', '-ifrunning', 'steam://open/keyboard')
    until(lambda: len(keys()) >= 24, 20, 0.5, 'keyboard over the editor')
    time.sleep(1.5)
    f = field()
    out['field_seen_by_layer'] = f
    out['layer_before'] = layer()
    until(lambda: strip()['strip'], 8, what='strip over the editor')
    out['strip_over_editor'] = strip()
    # stock taps first (they build the layer's memory), then a swipe
    for ch in 'we':
        tap_key(ch, 70); time.sleep(0.4)
    tap_key(' ', 70); time.sleep(0.5)
    out['memory_after_typing'] = layer()['s']
    t0 = time.time()
    swipe(list('hello'))
    until(lambda: layer()['s']['candidates'] > 0, 40, 0.05, 'candidates from the engine in unknown mode')
    out['seconds_swipe_to_candidates'] = round(time.time() - t0, 3)
    time.sleep(0.8)
    out['strip_after_swipe'] = strip()['strip']
    out['layer_after'] = layer()['s']
    return out


FIELD_VALUE = "document.activeElement && document.activeElement.value"


def cmd_forged():
    """A page message naming a candidate the engine never issued must type nothing (the helper refuses it; the engine does too)."""
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    swipe(list('world'), 300)
    until(lambda: field().get('value'), 20, 0.05, 'text')
    time.sleep(1.0)
    first = field()['value']
    msg = lambda cand, erase: json.dumps(json.dumps({'v': 1, 'type': 'pick', 'id': 1999999, 'req': 1, 'cand': cand, 'erase': erase}))
    for forged in ('0123456789abcdef', 'ffffffffffffffff', 'hello'):
        ev('window.__swipekb(%s)' % msg(forged, len(first)))
        time.sleep(0.8)
    after_forged = field()['value']
    bad_erase = field()['value']
    ev('window.__swipekb(%s)' % msg('0123456789abcdef', 1))
    time.sleep(0.8)
    out = {'typed_by_swipe': first, 'after_three_forged_picks': after_forged, 'after_wrong_erase': field()['value']}
    clear_field()
    return out


def cmd_shift():
    """Shift and Caps Lock around a swiped word (plan Section 7: decide what happens to Steam's Shift latch after a swipe commit)."""
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    out = {}

    def letters_upper():
        return ev("(function(){var n=document.querySelector('[data-key=q]');return n&&n.textContent.trim()})()")
    # one-shot Shift, then a swipe, then a plain tap
    tap_key('Shift', 70); time.sleep(1.0)
    out['q_label_after_shift_tap'] = letters_upper()
    swipe(list('world'), 300)
    until(lambda: field().get('value'), 20, 0.05, 'text')
    time.sleep(2.0)
    out['one_shot_swipe_field'] = field()['value']
    out['q_label_after_swipe'] = letters_upper()
    tap_key('a', 70); time.sleep(1.0)
    out['next_tapped_letter_field'] = field()['value']
    clear_field()
    # Caps Lock on, a swipe, Caps Lock stays, then off
    tap_key('CapsLock', 70); time.sleep(1.0)
    out['q_label_after_caps_tap'] = letters_upper()
    swipe(list('world'), 300)
    until(lambda: field().get('value'), 20, 0.05, 'text')
    time.sleep(2.0)
    out['caps_swipe_field'] = field()['value']
    out['q_label_after_caps_swipe'] = letters_upper()
    tap_key('CapsLock', 70); time.sleep(1.0)
    out['q_label_after_caps_off'] = letters_upper()
    clear_field()
    out['field_after_clear'] = field()['value']
    return out


def cmd_trail():
    """Start a slow swipe and return at once, so the host can take a picture while the finger is still moving."""
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    ks = keys()
    pts = ' '.join('%d,%d' % (ks[c][0], ks[c][1]) for c in 'qwertyuiop')
    subprocess.Popen(['python3', '-c', "import socket;s=socket.socket(socket.AF_UNIX);s.connect('/tmp/touchd.sock');f=s.makefile('rw');f.write('drag 4000 %s\\n');f.flush();f.readline()" % pts])
    time.sleep(1.6)
    return {'canvas_during_swipe': json.loads(ev("(function(){var c=document.querySelector('.swipekb-trail');return JSON.stringify(c?{w:c.width,h:c.height,pe:c.style.pointerEvents}:null)})()"))}


def cmd_trail_after():
    time.sleep(3.0)
    t0 = time.time()
    gone = until(lambda: ev("document.querySelector('.swipekb-trail') === null"), 3, 0.02, 'trail removed')
    time.sleep(2.0)
    v = field()['value']
    clear_field()
    return {'trail_removed': gone, 'field_value_after_slow_swipe': v}


def cmd_latency(n):
    """Time from the finger lifting (touchd answers when the drag is over) to the word appearing in the field, over n swipes."""
    open_field(); clear_field(); until(lambda: strip()['strip'], 5, what='strip')
    words = ['world', 'thanks', 'please', 'good', 'today', 'steam']
    out, parts = [], []
    for i in range(n):
        w = words[i % len(words)]
        n0 = len(field()['value'])
        swipe(list(w), 60 * len(w))
        t0 = time.time()
        until(lambda: len(field()['value']) > n0, 20, 0.01, 'text')
        out.append(round((time.time() - t0) * 1000))
        time.sleep(0.5)
        parts.append(layer()['s'].get('lastMs'))
        time.sleep(0.7)
        if len(field()['value']) > 30:
            clear_field()
    clear_field()
    xs = sorted(out)
    return {'ms_release_to_text': out, 'parts_wait_engine_send_ms': parts, 'median': xs[len(xs) // 2], 'min': xs[0], 'max': xs[-1]}


def cmd_taps(rounds):
    """Regression of M4 with the typing session on: every letter once per round must type exactly once (26 per round)."""
    open_field(); clear_field()
    letters = 'qwertyuiopasdfghjklzxcvbnm'
    ks = keys()
    exact, bad = 0, []
    for r in range(rounds):
        for c in letters:
            touch('tap %d %d 60' % (ks[c][0], ks[c][1]))
            time.sleep(0.25)
        v = until(lambda: len(field()['value']) >= len(letters) and field()['value'], 30, 0.2, 'all letters typed')
        time.sleep(1.0)
        v = field()['value']
        if v == letters:
            exact += 1
        else:
            bad.append({'round': r, 'got': v})
        out_mem = layer()['s']
        clear_field()
    return {'rounds': rounds, 'rounds_exact': exact, 'mismatches': bad, 'layer': layer(), 'strip_still_shown': bool(strip()['strip'])}


def cmd_kwrite_more():
    """A second keyboard session over the editor: it must start with NO memory of the first, then swipe three words and replace one."""
    out = {}
    sh('steam', '-ifrunning', 'steam://open/keyboard')
    until(lambda: len(keys()) >= 24, 20, 0.5, 'keyboard over the editor')
    until(lambda: strip()['strip'], 8, what='strip')
    time.sleep(1.0)
    out['memory_at_reopen'] = layer()['s']
    tap_key(' ', 70); time.sleep(0.5)
    words = []
    for w in ('world', 'thanks', 'good'):
        n0 = layer()['s']['bufferLength']
        swipe(list(w), 60 * len(w))
        until(lambda: layer()['s']['bufferLength'] > n0, 20, 0.05, 'word typed')
        time.sleep(1.0)
        words.append({'swiped': w, 'candidates_left_to_right': [x['text'] for x in strip()['strip']['slots']], 'memory': layer()['s']})
    slot = strip()['strip']['slots'][2]
    n0 = layer()['s']['bufferLength']
    touch('tap %d %d 80' % (slot['x'], slot['y']))
    time.sleep(1.5)
    out['tapped_candidate'] = slot['text']
    out['memory_after_pick'] = layer()['s']
    out['words'] = words
    return out


def cmd_kwrite_close():
    out = {'before': layer()['s']}
    sh('steam', '-ifrunning', 'steam://close/keyboard')
    t0 = time.time()
    try:
        until(lambda: layer()['s']['bufferLength'] == 0 and not layer()['s']['stripShown'], 10, 0.05, 'memory wiped')
    except SystemExit as e:
        out['error'] = str(e)
    out['seconds_to_forget'] = round(time.time() - t0, 3)
    out['after'] = layer()['s']
    out['keys_on_screen'] = len(keys())
    return out


def cmd_kwrite_down():
    out = {}
    appid = int(APPID_FILE.read_text()) if APPID_FILE.exists() else None
    out['appid'] = appid
    if appid is not None:
        try:
            ev('SteamClient.Apps.TerminateApp(%d, false)' % appid, title=SJS, await_promise=True)
        except Exception as e:                                         # the editor may already be closed
            out['terminate_note'] = str(e)[:100]
        time.sleep(2.0)
        sh('pkill', '-x', 'kwrite')
        ev('SteamClient.Apps.RemoveShortcut(%d)' % appid, title=SJS, await_promise=True)
        time.sleep(2.0)
    vdfs = list((HOME / '.local/share/Steam/userdata').glob('*/config/shortcuts.vdf'))
    out['shortcuts_vdf_mentions'] = sum(p.read_bytes().count(SHORTCUT.encode()) for p in vdfs)
    out['kwrite_running'] = bool(sh('pgrep', '-x', 'kwrite').stdout.strip())
    out['note_file_removed'] = False
    if NOTE.exists():
        NOTE.unlink()
        out['note_file_removed'] = True
    out['app_store_has_entry'] = ev('(function(){try{return JSON.stringify(Array.from(appStore.allApps||[]).filter(function(a){return a&&a.display_name===%s}).length)}catch(e){return "n/a:"+e}})()' % json.dumps(SHORTCUT), title=SJS)
    if out['shortcuts_vdf_mentions'] == 0 and not out['kwrite_running']:
        APPID_FILE.unlink(missing_ok=True)
    return out


def cmd_fs(a):
    snap = Path(HOME / 'm5' / ('fs-%s.json' % a[1]))
    if a[0] == 'snap':
        snap.write_text(json.dumps(fs_listing(), indent=1, sort_keys=True))
        return {'files_listed': len(fs_listing()), 'dirs': [str(d) for d in watched_dirs()]}
    b = json.loads((HOME / 'm5' / ('fs-%s.json' % a[1])).read_text())
    c = json.loads((HOME / 'm5' / ('fs-%s.json' % a[2])).read_text())
    d = {k: [b.get(k), c.get(k)] for k in set(b) | set(c) if b.get(k) != c.get(k)}
    return {'identical': not d, 'files_compared': len(b), 'differences': d}


def main():
    c = sys.argv[1]
    a = sys.argv[2:]
    out = {'mode': lambda: mode(a[0]), 'hello': cmd_hello, 'scales': cmd_scales, 'private': cmd_private, 'password': cmd_password,
           'kwrite-up': cmd_kwrite_up, 'kwrite-run': cmd_kwrite_run, 'kwrite-close': cmd_kwrite_close, 'kwrite-more': cmd_kwrite_more, 'forged': cmd_forged, 'taps': lambda: cmd_taps(int(a[0])), 'shift': cmd_shift, 'trail': cmd_trail, 'trail-after': cmd_trail_after, 'latency': lambda: cmd_latency(int(a[0])), 'kwrite-down': cmd_kwrite_down,
           'fs': lambda: cmd_fs(a), 'layer': layer, 'clear': clear_field}[c]()
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
