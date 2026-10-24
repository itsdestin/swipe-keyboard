#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""M4 acceptance rig for the gesture arbiter. Runs INSIDE the SteamOS Gaming Mode guest as the plain user (plan Section 14, M4).

Deploy with deploy-m4.sh (helper, layer, contract and this file go to ~/m4). Needs touchd.py running as root in the guest
(tools/gaming-vm/touchd.py, socket /tmp/touchd.sock) and the guest's Steam signed in with the debug port open.

Usage: python3 ~/m4/tools/gaming-vm/m4_acceptance.py <command> [args]
  mode on|off        on = helper running, layer installed with gestures 'on'; off = helper stopped, layer removed
  field              open the search page and its keyboard, report the focused field's type
  taps N             every letter once per round, N rounds, then Backspace the field empty. Prints counts + latencies
  swipes N           N scripted swipes of varied speed; checks no letters appear; counts the strokes the page sent out
  shift              Shift then letter (sequential and two-finger); capital check
  backspace          Backspace tap and a 1.5 s hold: timestamps of every keydown
  popup              hold 'e' 0.9 s: DOM of the accent popup, when it appeared, what typing came out
  longtouch          3 s still touch on a letter, and one 3 s slow swipe; both must release cleanly
  fault              throw inside a handler: layer must remove its listeners and the next tap must type
  clear              empty the field with Backspace taps

WHY it measures in the page: the page's own clock stamps the real finger-up and the keyboard's key press, so the
release-to-keydown number is the same quantity with the layer on and off, with no host/guest skew.
Account safety: only the search box is typed in (a local filter), never Enter, and everything typed is deleted again.
"""
import json
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path

HOME = Path.home()
sys.path.insert(0, str(HOME / 'm4' / 'gaming' / 'helper'))
from swipekb_helper import cdp, ws  # noqa: E402

PORT = 8080
BP = 'Steam Big Picture Mode'
UNIT = 'swipekb-m4-helper'
LETTERS = 'qwertyuiopasdfghjklzxcvbnm'
STATUS = HOME / '.local' / 'state' / 'swipekb-gaming' / 'status.json'

_c = None


def sh(*a):
    return subprocess.run(a, capture_output=True, text=True)


def bp_id():
    for t in cdp.http_json(PORT, '/json/list'):
        if t.get('type') == 'page' and t.get('title') == BP:
            return t['id']
    raise SystemExit('no Big Picture page')


def ev(js, timeout=5.0):
    global _c
    for attempt in (0, 1):
        try:
            if _c is None:
                _c = cdp.Cdp.open(PORT, cdp.page_path(bp_id()), timeout=timeout)
            return _c.evaluate(js, timeout=timeout)
        except (ws.WsError, OSError, cdp.CdpError):
            _c = None
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


# ---- the page-side logger: stamps every real and replayed event with the page clock ---------------------------
LOGGER = r"""
(function(){
 if (window.__m4) return 'have';
 var L = window.__m4 = {ev: [], popup: [], t0: performance.now()};
 function rec(e){ L.ev.push([e.type, +(performance.now()).toFixed(1), e.key||'', Math.round(e.clientX||0), Math.round(e.clientY||0), e.isTrusted?1:0]); }
 ['pointerdown','pointerup','pointermove','touchstart','touchend','keydown','keyup'].forEach(function(t){ window.addEventListener(t, rec, true); });
 var last = 0;
 new MutationObserver(function(){
   var n = document.querySelectorAll('[data-key-is-literal]').length;
   if (n !== last) { L.popup.push([+(performance.now()).toFixed(1), n]); last = n; }
 }).observe(document.body, {subtree: true, childList: true});
 return 'installed';
})()
"""

KEYS_JS = r"""
(function(){
 var out = {}, nodes = document.querySelectorAll('[data-key]');
 for (var i = 0; i < nodes.length; i++) {
   var n = nodes[i];
   if (n.hasAttribute('data-key-is-literal')) continue;
   if (n.parentElement && n.parentElement.closest('[data-key]')) continue;
   var r = n.getBoundingClientRect();
   out[n.getAttribute('data-key')] = [Math.round(r.left + r.width/2), Math.round(r.top + r.height/2), Math.round(r.width), Math.round(r.height)];
 }
 return JSON.stringify(out);
})()
"""
FIELD_JS = ("(function(){var a=document.activeElement;return JSON.stringify({tag:a&&a.tagName,type:a&&a.type,value:a&&a.value})})()")
RESET_LOG = "window.__m4 && (window.__m4.ev = [], window.__m4.popup = [], window.__m4.t0 = performance.now(), 1)"
GET_LOG = "JSON.stringify(window.__m4)"
LAYER_JS = ("JSON.stringify(window.__swipekbLayer ? {v: window.__swipekbLayer.version, g: window.__swipekbLayer.gestures ? "
            "window.__swipekbLayer.gestures.state() : null, stats: window.__swipekbLayer.gestures ? window.__swipekbLayer.gestures.stats() : null,"
            " attr: document.documentElement.getAttribute('data-swipekb-gestures')} : null)")


def quiet(expect=None, still=1.0, limit=15.0):
    """Wait until Steam has finished typing. WHY: on a busy host Steam's typing stalls and then delivers key presses up to
    ~2 s late (seen with the layer OFF too), so a fixed sleep reads the field too early. With `expect` (the number of key
    presses this gesture should cause, counted since the log was last reset) we wait for them, then for `still` seconds of
    quiet to catch a second, unwanted press (a double type). Without it we only wait for quiet, with a longer `still`."""
    end = time.time() + limit
    last, since = -1, time.time()
    while time.time() < end:
        evs = log()['ev']
        n = sum(1 for e in evs if e[0] == 'keydown')
        if n != last:
            last, since = n, time.time()
        need = (expect is None) or n >= expect
        if need and time.time() - since >= (still if expect is not None else max(still, 2.0)):
            return n
        time.sleep(0.1)
    return last


def keys():
    return {k: v for k, v in json.loads(ev(KEYS_JS)).items()}


def field():
    return json.loads(ev(FIELD_JS))


def log():
    return json.loads(ev(GET_LOG))


def layer():
    return json.loads(ev(LAYER_JS))


def pairs(events):
    """-> list of (release time of the REAL finger-up, key pressed, ms from release to keydown), one per keydown.
    WHY by order and not "the up just before": Steam sometimes types a burst late, so the n-th real finger-up is matched
    with the n-th key press (every tap in these tests types exactly one key)."""
    ups = [e[1] for e in events if e[0] == 'pointerup' and e[5]]
    kds = [e for e in events if e[0] == 'keydown']
    return [(u, k[2], round(k[1] - u, 1)) for u, k in zip(ups, kds)]


def summarize(xs):
    xs = sorted(xs)
    if not xs:
        return {}
    return {'n': len(xs), 'median': round(statistics.median(xs), 1), 'mean': round(statistics.mean(xs), 1),
            'p95': xs[int(len(xs) * .95) - 1 if len(xs) > 1 else 0], 'min': xs[0], 'max': xs[-1]}


# ---- helper / layer mode -----------------------------------------------------------------------------------------
def helper_unit_active():
    return sh('systemctl', '--user', 'is-active', UNIT).stdout.strip() == 'active'


def mode(which):
    if which == 'off':
        sh('systemctl', '--user', 'stop', UNIT)
        t = time.time()
        while time.time() - t < 10:
            l = layer()
            if l is None:
                return {'mode': 'off', 'layer': None}
            time.sleep(0.1)
        raise SystemExit('layer still present after helper stop: %s' % l)
    ev(LOGGER)            # the logger goes in BEFORE the layer, so it sees every real event first
    if not helper_unit_active():
        sh('systemctl', '--user', 'reset-failed', UNIT)
        r = sh('systemd-run', '--user', '--unit=' + UNIT, '--collect', '--setenv=PYTHONPATH=%s' % (HOME / 'm4' / 'gaming' / 'helper'),
               '--setenv=PYTHONDONTWRITEBYTECODE=1', '-p', 'Restart=always', '-p', 'RestartSec=3', 'python3', '-m', 'swipekb_helper')
        if r.returncode:
            raise SystemExit(r.stderr)
    t = time.time()
    while time.time() - t < 15:
        try:
            l = layer()
            if l and l['g'] == 'on':
                return {'mode': 'on', 'layer': l}
        except (ws.WsError, OSError, cdp.CdpError):
            pass
        time.sleep(0.1)
    raise SystemExit('layer did not come up')


def ensure_logger():
    ev(LOGGER)


def tap_key(name, hold=60):
    k = keys()[name]
    touch('tap %d %d %d' % (k[0], k[1], hold))


def open_field():
    f = field()
    if f.get('type') == 'search' and len(keys()) >= 24:
        return f
    touch('tap 1039 20')
    time.sleep(2.0)
    f = field()
    if f.get('type') != 'search' or len(keys()) < 24:
        raise SystemExit('could not open a search field with the keyboard: %s' % f)
    return f


def clear_field(max_taps=80):
    n = 0
    ev(RESET_LOG); quiet()
    for _ in range(4):
        left = len(field().get('value') or '')
        if not left:
            break
        ev(RESET_LOG)
        for _i in range(min(left, max_taps)):
            tap_key('Backspace', 90)
            time.sleep(0.25)
            n += 1
        quiet(expect=min(left, max_taps))
    return {'cleared_with_backspaces': n, 'value': field().get('value')}


# ---- commands ----------------------------------------------------------------------------------------------------
def cmd_taps(rounds):
    open_field(); ensure_logger()
    ks = keys()
    assert all(c in ks for c in LETTERS), 'letters missing from the keyboard'
    clear_field()
    ok = 0; bad = []; lat = []; fast = 0
    for r in range(rounds):
        ev(RESET_LOG)
        for c in LETTERS:
            touch('tap %d %d 60' % (ks[c][0], ks[c][1]))
            time.sleep(0.25)
        quiet(expect=len(LETTERS))
        v = field().get('value')
        if v == LETTERS:
            ok += 1
        else:
            bad.append({'round': r, 'got': v})
        ds = [p[2] for p in pairs(log()['ev'])]
        lat += ds
        clear_field()
    s = summarize(lat)
    s['share_under_100ms'] = round(sum(1 for x in lat if x < 100) / len(lat), 3) if lat else None
    s['p25'] = sorted(lat)[len(lat) // 4] if lat else None
    return {'rounds': rounds, 'rounds_exact': ok, 'mismatches': bad, 'release_to_keydown_ms': s, 'samples': lat, 'layer': layer()}


SWIPE_PATHS = [('q', 'p'), ('a', 'l'), ('z', 'm'), ('h', 'e', 'l', 'l', 'o'), ('t', 'o', 'p'), ('g', 'y'), ('w', 'o', 'r', 'd'),
               ('p', 'q'), ('m', 'z'), ('s', 'k')]
DURATIONS = [30, 60, 100, 200, 400, 800]


def drain(c, strokes, timeout):
    while True:
        try:
            m = c.next_event(timeout=timeout)
        except ws.Timeout:
            return
        if m.get('method') == 'Runtime.bindingCalled' and m['params'].get('name') == '__swipekb':
            strokes.append(json.loads(m['params']['payload']))


def cmd_swipes(n):
    open_field(); ensure_logger(); clear_field()
    ks = keys()
    c = cdp.Cdp.open(PORT, cdp.page_path(bp_id()), timeout=5.0)
    strokes = []
    try:
        c.call('Runtime.addBinding', {'name': '__swipekb'})
        c.call('Runtime.enable')
        ev(RESET_LOG)
        for i in range(n):
            path = SWIPE_PATHS[i % len(SWIPE_PATHS)]
            pts = ' '.join('%d,%d' % (ks[ch][0], ks[ch][1]) for ch in path)
            touch('drag %d %s' % (DURATIONS[i % len(DURATIONS)], pts))
            time.sleep(0.2)
            drain(c, strokes, 0.05)
        quiet(expect=None)          # a late stray letter would show up in this wait
        drain(c, strokes, 0.3)
    finally:
        c.close()
    evs = log()['ev']
    res = {'swipes': n, 'field_value_after': field().get('value'), 'keydowns_after_swipes': [e for e in evs if e[0] == 'keydown'],
           'strokes_delivered_to_binding': len(strokes),
           'stroke_points_min_median_max': [min(len(s['trace']) for s in strokes), statistics.median(len(s['trace']) for s in strokes),
                                             max(len(s['trace']) for s in strokes)] if strokes else None,
           'keys_in_each_stroke': sorted({len(s['keys']) for s in strokes}), 'layer': layer()}
    clear_field()
    return res


def cmd_shift():
    open_field(); ensure_logger(); ks = keys(); res = {}
    clear_field()
    sh_, h = ks['Shift'], ks['h']
    for rep in range(3):
        # sequential: tap Shift, then tap h   (Shift press + H press = 2 key presses)
        ev(RESET_LOG)
        touch('tap %d %d 60' % tuple(sh_[:2])); time.sleep(0.4); touch('tap %d %d 60' % tuple(h[:2])); quiet(expect=2)
        res.setdefault('sequential_shift_then_h', []).append(field().get('value')); clear_field()
        # two fingers: finger 1 holds Shift, finger 2 taps h
        ev(RESET_LOG)
        touch('down 0 %d %d' % (sh_[0], sh_[1]), 'sleep 120', 'down 1 %d %d' % (h[0], h[1]), 'sleep 80', 'up 1', 'sleep 80', 'up 0')
        quiet(expect=2)
        res.setdefault('two_finger_shift_h', []).append(field().get('value')); clear_field()
        # the latch must not stay armed: the next plain tap is lowercase
        ev(RESET_LOG)
        touch('tap %d %d 60' % tuple(ks['i'][:2])); quiet(expect=1)
        res.setdefault('next_tap_after_two_finger', []).append(field().get('value')); clear_field()
    res['layer'] = layer()
    return res


def cmd_backspace():
    open_field(); ensure_logger(); ks = keys(); k = ks['Backspace']
    clear_field()
    taps = []
    for i in range(12):
        ev(RESET_LOG)
        for c in 'ab':
            tap_key(c); time.sleep(0.25)
        quiet(expect=2)
        ev(RESET_LOG)
        touch('tap %d %d 60' % (k[0], k[1])); quiet(expect=1)
        taps += [p[2] for p in pairs(log()['ev'])]
        clear_field()
    holds = []
    for i in range(3):
        for c in 'abcdefghij':
            tap_key(c); time.sleep(0.25)
        quiet(expect=None)
        ev(RESET_LOG)
        touch('down 0 %d %d' % (k[0], k[1]), 'sleep 1500', 'up 0'); quiet(expect=None)
        hold = log()['ev']
        down = next(e[1] for e in hold if e[0] == 'pointerdown')
        holds.append([round(e[1] - down, 1) for e in hold if e[0] == 'keydown'])
        clear_field()
    return {'tap_release_to_keydown_ms': summarize(taps), 'tap_samples': taps,
            'hold_keydowns_ms_after_press_per_trial': holds, 'hold_repeat_counts': [len(h) for h in holds],
            'first_repeat_ms': [h[0] for h in holds if h], 'layer': layer()}


def cmd_popup():
    open_field(); ensure_logger(); ks = keys(); e = ks['e']
    clear_field(); res = {'trials': []}
    for trial in range(3):
        ev(RESET_LOG)
        touch('down 0 %d %d' % (e[0], e[1]), 'sleep 950')
        snap = ev("""JSON.stringify((function(){var o=[...document.querySelectorAll('[data-key-is-literal]')].map(function(n){var r=n.getBoundingClientRect();
          return [n.getAttribute('data-key'), n.className, Math.round(r.left), Math.round(r.top), Math.round(r.width)]});
          var p=document.querySelector('[data-key-is-literal]'); return {items:o, parentClass: p&&p.parentElement? p.parentElement.className:null}})())""")
        touch('up 0'); quiet(expect=1)
        lg = log()
        down = next(x[1] for x in lg['ev'] if x[0] == 'pointerdown' and x[5])
        res['trials'].append({'popup': json.loads(snap), 'appeared_ms_after_real_touch': [round(p[0] - down, 1) for p in lg['popup'] if p[1] > 0],
                              'value_after_release_on_base_key': field().get('value'),
                              'popup_gone_after': ev("document.querySelectorAll('[data-key-is-literal]').length")})
        clear_field()
    # slide onto an accent: glide in small steps from the key to the item (a single jump is not a slide for Steam)
    ev(RESET_LOG)
    touch('down 0 %d %d' % (e[0], e[1]), 'sleep 950')
    third = json.loads(ev("JSON.stringify((function(){var n=document.querySelectorAll('[data-key-is-literal]')[2];var r=n.getBoundingClientRect();return [Math.round(r.left+r.width/2),Math.round(r.top+r.height/2),n.getAttribute('data-key')]})())"))
    steps = 8
    cmds = []
    for i in range(1, steps + 1):
        cmds += ['move 0 %d %d' % (e[0] + (third[0] - e[0]) * i // steps, e[1] + (third[1] - e[1]) * i // steps), 'sleep 25']
    touch(*cmds, 'sleep 200', 'up 0'); quiet(expect=1)
    res['slide_to'] = third[2]; res['value_after_slide'] = field().get('value')
    res['layer'] = layer()
    clear_field()
    return res


def cmd_longtouch():
    open_field(); ensure_logger(); ks = keys(); clear_field(); res = {}
    d = ks['d']
    ev(RESET_LOG)
    t = time.time()
    touch('down 0 %d %d' % (d[0], d[1]), 'sleep 3000', 'up 0')
    res['touch_returned_after_s'] = round(time.time() - t, 2)
    quiet(expect=1)
    res['still_3s_touch_value'] = field().get('value')
    res['pointerdown_to_pointerup_ms'] = [round(b[1] - a[1]) for a, b in zip([x for x in log()['ev'] if x[0] == 'pointerdown' and x[5]], [x for x in log()['ev'] if x[0] == 'pointerup' and x[5]])]
    clear_field()
    a, l = ks['a'], ks['l']
    ev(RESET_LOG)
    touch('drag 3000 %d,%d %d,%d' % (a[0], a[1], l[0], l[1])); quiet(expect=None)
    res['slow_3s_swipe_value'] = field().get('value')
    clear_field()
    f = ks['f']; ev(RESET_LOG); touch('tap %d %d 60' % (f[0], f[1])); quiet(expect=1)
    res['next_tap_value'] = field().get('value')
    res['layer'] = layer()
    clear_field()
    return res


def cmd_bslat(n):
    open_field(); ensure_logger(); k = keys()['Backspace']
    clear_field(); ev(RESET_LOG)
    for i in range(n):
        touch('tap %d %d 60' % (k[0], k[1]))
        time.sleep(0.5)
    quiet(expect=n)
    ds = [p[2] for p in pairs(log()['ev'])]
    return {'taps': n, 'keydowns': len(ds), 'release_to_keydown_ms': summarize(ds), 'samples': ds, 'layer': layer()}


def helper_pid():
    r = sh('systemctl', '--user', 'show', '-p', 'MainPID', '--value', UNIT)
    return int(r.stdout.strip() or 0)


def cmd_dead():
    """The helper (and with it everything behind it: engine, binding) stops answering. The layer must still let every touch
    through the normal way and, after its 6 s watchdog, take itself out; stock typing must work throughout."""
    import os
    import signal
    open_field(); ensure_logger(); ks = keys(); clear_field(); res = {}
    pid = helper_pid()
    assert pid, 'helper not running'
    froze = time.time()
    os.kill(pid, signal.SIGSTOP)           # frozen: no heartbeat, no answers; its systemd Restart= does not fire
    try:
        d = ks['d']; ev(RESET_LOG)
        t = time.time()
        touch('down 0 %d %d' % (d[0], d[1]), 'sleep 3000', 'up 0')
        res['touch_returned_after_s'] = round(time.time() - t, 2)
        a, l = ks['a'], ks['l']          # a swipe straight after, still inside the layer's 6 s watchdog window
        touch('drag 200 %d,%d %d,%d' % (a[0], a[1], l[0], l[1]))
        res['swipe_started_s_after_freeze'] = round(time.time() - froze - 0.2, 1)
        while time.time() - froze < 14 and layer() is not None:
            time.sleep(0.25)
        gone = layer() is None
        res['layer_removed_itself_s_after_freeze'] = round(time.time() - froze, 1) if gone else None
        quiet(expect=1)
        res['value_3s_touch_plus_swipe_with_helper_frozen'] = field().get('value')   # 'd' only: the swipe left no letter
        clear_field()
        f = ks['f']; ev(RESET_LOG); touch('tap %d %d 60' % (f[0], f[1])); quiet(expect=1)
        res['tap_after_layer_gone'] = field().get('value')
    finally:
        os.kill(pid, signal.SIGCONT)
    t = time.time()
    while time.time() - t < 15:
        l = layer()
        if l and l['g'] == 'on':
            break
        time.sleep(0.25)
    res['layer_back_after_helper_resumed_s'] = round(time.time() - t, 1)
    res['layer_final'] = layer()
    clear_field()
    return res


def cmd_fault():
    open_field(); ensure_logger(); ks = keys(); clear_field(); res = {}
    ev(RESET_LOG)
    ev('window.__swipekbLayer.gestures.injectFault()')
    g = ks['g']; touch('tap %d %d 60' % (g[0], g[1])); quiet(expect=1)
    res['tap_while_fault_fires'] = field().get('value')
    res['layer_after_fault'] = layer()
    res['reason'] = ev('window.__swipekbLayer.gestures.reason()')
    ev(RESET_LOG)
    h = ks['h']; touch('tap %d %d 60' % (h[0], h[1])); quiet(expect=1)
    res['value_after_next_tap'] = field().get('value')
    ev(RESET_LOG)
    # a swipe now must behave like stock Steam (the layer is gone): a stray letter is the expected, honest proof
    a, l = ks['a'], ks['l']
    touch('drag 200 %d,%d %d,%d' % (a[0], a[1], l[0], l[1])); quiet(expect=1)
    res['value_after_swipe_with_layer_removed'] = field().get('value')
    time.sleep(2.5)
    res['layer_2s_later'] = layer()
    res['status_file'] = json.loads(STATUS.read_text()) if STATUS.exists() else None
    clear_field()
    return res


def main():
    c = sys.argv[1]
    a = sys.argv[2:]
    out = {'mode': lambda: mode(a[0]), 'field': lambda: open_field(), 'taps': lambda: cmd_taps(int(a[0])),
           'swipes': lambda: cmd_swipes(int(a[0])), 'shift': cmd_shift, 'backspace': cmd_backspace, 'popup': cmd_popup,
           'longtouch': cmd_longtouch, 'fault': cmd_fault, 'bslat': lambda: cmd_bslat(int(a[0])), 'dead': cmd_dead, 'clear': lambda: clear_field(), 'layer': layer}[c]()
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
