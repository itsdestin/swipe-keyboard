#!/usr/bin/env python3
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


def keys():
    return {k: v for k, v in json.loads(ev(KEYS_JS)).items()}


def field():
    return json.loads(ev(FIELD_JS))


def log():
    return json.loads(ev(GET_LOG))


def layer():
    return json.loads(ev(LAYER_JS))


def pairs(events):
    """-> list of (release time of the REAL finger-up, key pressed, ms from release to keydown) for letter keydowns."""
    out, last_up = [], None
    for e in events:
        if e[0] == 'pointerup' and e[5]:
            last_up = e[1]
        elif e[0] == 'keydown' and last_up is not None:
            out.append((last_up, e[2], round(e[1] - last_up, 1)))
            last_up = None
    return out


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
    while field().get('value') and n < max_taps:
        tap_key('Backspace', 50)
        time.sleep(0.12)
        n += 1
    time.sleep(0.2)
    return {'cleared_with_backspaces': n, 'value': field().get('value')}


# ---- commands ----------------------------------------------------------------------------------------------------
def cmd_taps(rounds):
    open_field(); ensure_logger()
    ks = keys()
    assert all(c in ks for c in LETTERS), 'letters missing from the keyboard'
    ok = 0; bad = []; lat = []
    ev(RESET_LOG)
    for r in range(rounds):
        before = len(log()['ev'])
        for c in LETTERS:
            touch('tap %d %d 60' % (ks[c][0], ks[c][1]))
            time.sleep(0.11)
        time.sleep(0.4)
        v = field().get('value')
        if v == LETTERS:
            ok += 1
        else:
            bad.append({'round': r, 'got': v})
        evs = log()['ev'][before:]
        lat += [p[2] for p in pairs(evs) if p[1] in LETTERS]
        clear_field()
    return {'rounds': rounds, 'rounds_exact': ok, 'mismatches': bad, 'release_to_keydown_ms': summarize(lat), 'layer': layer()}


SWIPE_PATHS = [('q', 'p'), ('a', 'l'), ('z', 'm'), ('h', 'e', 'l', 'l', 'o'), ('t', 'o', 'p'), ('g', 'y'), ('w', 'o', 'r', 'd'),
               ('p', 'q'), ('m', 'z'), ('s', 'k')]
DURATIONS = [30, 60, 100, 200, 400, 800]


def cmd_swipes(n):
    open_field(); ensure_logger()
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
            time.sleep(0.15)
            # collect whatever the binding delivered
            while True:
                try:
                    m = c.next_event(timeout=0.05)
                except ws.Timeout:
                    break
                if m.get('method') == 'Runtime.bindingCalled' and m['params'].get('name') == '__swipekb':
                    strokes.append(json.loads(m['params']['payload']))
        time.sleep(0.5)
        while True:
            try:
                m = c.next_event(timeout=0.2)
            except ws.Timeout:
                break
            if m.get('method') == 'Runtime.bindingCalled':
                strokes.append(json.loads(m['params']['payload']))
    finally:
        c.close()
    evs = log()['ev']
    return {'swipes': n, 'field_value_after': field().get('value'), 'keydowns_after_swipes': [e for e in evs if e[0] == 'keydown'],
            'strokes_delivered_to_binding': len(strokes),
            'stroke_points_min_median_max': [min(len(s['trace']) for s in strokes), statistics.median(len(s['trace']) for s in strokes),
                                              max(len(s['trace']) for s in strokes)] if strokes else None,
            'keys_in_each_stroke': sorted({len(s['keys']) for s in strokes}), 'layer': layer()}


def cmd_shift():
    open_field(); ensure_logger(); ks = keys(); res = {}
    clear_field()
    # sequential: tap Shift, then tap h
    touch('tap %d %d 60' % tuple(ks['Shift'][:2])); time.sleep(0.4); touch('tap %d %d 60' % tuple(ks['h'][:2])); time.sleep(0.5)
    res['sequential_shift_then_h'] = field().get('value'); clear_field()
    # two fingers: finger 1 holds Shift, finger 2 taps h
    s, h = ks['Shift'], ks['h']
    touch('down 0 %d %d' % (s[0], s[1]), 'sleep 120', 'down 1 %d %d' % (h[0], h[1]), 'sleep 80', 'up 1', 'sleep 80', 'up 0')
    time.sleep(0.6)
    res['two_finger_shift_h'] = field().get('value'); clear_field()
    # latch must not stay armed: next plain tap is lowercase
    touch('tap %d %d 60' % tuple(ks['i'][:2])); time.sleep(0.4)
    res['next_tap_after_two_finger'] = field().get('value'); clear_field()
    res['layer'] = layer()
    return res


def cmd_backspace():
    open_field(); ensure_logger(); ks = keys()
    for c in 'abc':
        tap_key(c); time.sleep(0.15)
    time.sleep(0.3)
    ev(RESET_LOG)
    k = ks['Backspace']
    touch('tap %d %d 60' % (k[0], k[1])); time.sleep(0.5)
    tapev = log()['ev']
    ev(RESET_LOG)
    t0 = log()['t0']
    touch('down 0 %d %d' % (k[0], k[1]), 'sleep 1500', 'up 0'); time.sleep(0.5)
    hold = log()['ev']
    down = next(e[1] for e in hold if e[0] == 'pointerdown')
    kds = [round(e[1] - down, 1) for e in hold if e[0] == 'keydown']
    res = {'tap_release_to_keydown_ms': [p[2] for p in pairs(tapev)],
           'hold_keydowns_ms_after_press': kds, 'hold_repeat_count': len(kds),
           'value_after': field().get('value'), 'layer': layer()}
    clear_field()
    return res


def cmd_popup():
    open_field(); ensure_logger(); ks = keys()
    clear_field(); ev(RESET_LOG)
    e = ks['e']
    t_before = ev('performance.now()')
    touch('down 0 %d %d' % (e[0], e[1]), 'sleep 950')
    snap = ev("""JSON.stringify((function(){var o=[...document.querySelectorAll('[data-key-is-literal]')].map(function(n){var r=n.getBoundingClientRect();
      return [n.getAttribute('data-key'), n.className, Math.round(r.left), Math.round(r.top), Math.round(r.width)]});
      var p=document.querySelector('[data-key-is-literal]'); return {items:o, parentClass: p&&p.parentElement? p.parentElement.className:null}})())""")
    touch('up 0'); time.sleep(0.6)
    lg = log()
    down = next(x[1] for x in lg['ev'] if x[0] == 'pointerdown' and x[5])
    appeared = [round(p[0] - down, 1) for p in lg['popup'] if p[1] > 0]
    res = {'popup': json.loads(snap), 'popup_appeared_ms_after_real_touch': appeared, 'value_after_release_on_base_key': field().get('value'),
           'popup_gone_after': ev("document.querySelectorAll('[data-key-is-literal]').length"), 'layer': layer()}
    clear_field()
    # slide onto the third item and release
    touch('down 0 %d %d' % (e[0], e[1]), 'sleep 950')
    third = json.loads(ev("JSON.stringify((function(){var n=document.querySelectorAll('[data-key-is-literal]')[2];var r=n.getBoundingClientRect();return [Math.round(r.left+r.width/2),Math.round(r.top+r.height/2),n.getAttribute('data-key')]})())"))
    touch('move 0 %d %d' % (third[0], third[1]), 'sleep 150', 'up 0'); time.sleep(0.6)
    res['slide_to'] = third[2]; res['value_after_slide'] = field().get('value')
    clear_field()
    return res


def cmd_longtouch():
    open_field(); ensure_logger(); ks = keys(); clear_field(); res = {}
    d = ks['d']
    ev(RESET_LOG)
    t = time.time()
    touch('down 0 %d %d' % (d[0], d[1]), 'sleep 3000', 'up 0')
    time.sleep(0.7)
    res['still_3s_touch_value'] = field().get('value')
    res['elapsed_s'] = round(time.time() - t, 2)
    clear_field()
    a, l = ks['a'], ks['l']
    touch('drag 3000 %d,%d %d,%d' % (a[0], a[1], l[0], l[1])); time.sleep(0.7)
    res['slow_3s_swipe_value'] = field().get('value')
    f = ks['f']; touch('tap %d %d 60' % (f[0], f[1])); time.sleep(0.5)
    res['next_tap_value'] = field().get('value')
    res['layer'] = layer()
    clear_field()
    return res


def cmd_fault():
    open_field(); ensure_logger(); ks = keys(); clear_field(); res = {}
    ev('window.__swipekbLayer.gestures.injectFault()')
    g = ks['g']; touch('tap %d %d 60' % (g[0], g[1])); time.sleep(0.6)
    res['tap_while_fault_fires'] = field().get('value')
    res['layer_after_fault'] = layer()
    res['reason'] = ev('window.__swipekbLayer.gestures.reason()')
    h = ks['h']; touch('tap %d %d 60' % (h[0], h[1])); time.sleep(0.5)
    res['next_tap'] = field().get('value')
    # the frame stays (heartbeat keeps answering) and no new layer appears by itself
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
           'longtouch': cmd_longtouch, 'fault': cmd_fault, 'clear': lambda: clear_field(), 'layer': layer}[c]()
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
