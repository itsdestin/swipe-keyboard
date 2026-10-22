#!/usr/bin/env python3
"""M6 / M7-layer acceptance rig. Runs INSIDE the SteamOS Gaming Mode guest as the plain user (plan Section 14, M6 and M7).

Reuses the M5 rig (deploy-m6.sh puts both files in ~/m5): touchd.py running as root in the guest, the host half running
(m5_host.py up, with SWIPEKB_LOCALES listing the languages to test) and the helper switched on (m5_acceptance.py mode on).

Usage: python3 ~/m5/tools/gaming-vm/m6_acceptance.py <command> [args]
  layout-state          Steam's key grid class, the layer's view, the key names on screen
  switch NAME           press Steam's layout key until the grid says Layout_NAME (the layouts must already be active in Steam's settings)
  words NAME [N]        swipe N real words of the language of layout NAME in the search box; report top-3 and what was typed
  off NAME              a layout the engine does not serve: counting spies, stock taps, a stock swipe, no strip, nothing sent
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
 S.strokeBefore = window.__swipekbLayer && window.__swipekbLayer.lastStroke ? window.__swipekbLayer.lastStroke.id : null;
 return 'on';
})()
"""
SPY_READ = "JSON.stringify({stops: window.__m6spy.stops, binding: window.__m6spy.binding, strips: document.querySelectorAll('.swipekb-strip').length, trails: document.querySelectorAll('.swipekb-trail').length, lastStroke: window.__swipekbLayer && window.__swipekbLayer.lastStroke ? window.__swipekbLayer.lastStroke.id : null, strokeBefore: window.__m6spy.strokeBefore})"
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


def main():
    c, a = sys.argv[1], sys.argv[2:]
    out = {'layout-state': cmd_layout_state, 'switch': lambda: cmd_switch(a[0]), 'words': lambda: cmd_words(a[0], int(a[1]) if len(a) > 1 else 20),
           'off': lambda: cmd_off(a[0]), 'spacewar-up': cmd_spacewar_up, 'spacewar-down': cmd_spacewar_down}[c]()
    print(json.dumps(out, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
