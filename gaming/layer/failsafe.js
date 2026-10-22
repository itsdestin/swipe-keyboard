// Fail-safe watcher for the page layer (plan Section 10 and milestone M9).
//
// WHAT IT DOES, in plain words. The arbiter (gesture.js) and the typing session (session.js) only ever look at Steam's keyboard
// through the keys' attributes. If Valve changes those, the layer would quietly do nothing, or worse, hold back a touch it
// cannot make sense of. This small watcher answers two questions every half second, using only what the page already shows:
//   1. Do the keys on screen still carry the attributes the shared contract names (data-key-row, data-key-col, data-keycode)?
//      If a key is missing one, the layout is not what we measured: the layer takes ITS OWN listeners off (Steam's keyboard
//      is then plain stock) and says why.
//   2. Is the keyboard ever showing the 24 letter keys the arbiter needs, and does a keyboard ever show up at all when a text
//      box is focused? Those two cannot be proven wrong from one look (a number pad or symbols page has no letters; a text box
//      can be focused without a keyboard), so they are only COUNTED over several keyboard openings and reported. Reporting
//      never changes how the keyboard behaves: with fewer than 24 letter keys, or with no keys found, the arbiter already
//      passes every touch straight to Steam.
// The helper reads the report (L.report) with its once-a-second ping and writes the reason into the status file. Nothing
// here is sent anywhere else and nothing typed is ever looked at. This file defines ONE function; the frame calls it.
function swipekbFailsafe(w, L, opts) {
  'use strict';
  opts = opts || {};
  var doc = w.document;
  if (!doc || typeof w.setInterval !== 'function') return null;
  var dom = (L && L.config && L.config.keyboard_dom) || {};
  var KEY_SEL = dom.key_selector || '[data-key]';
  var REQUIRED = dom.required_attributes || [];
  var MIN_LETTERS = dom.min_letter_keys || 24;
  var LITERAL = 'data-key-is-literal';          // accent pop-up items: not part of the layout, never checked
  var TICK_MS = opts.tick_ms || 500;
  // WHY 3 everywhere: the same "three on the same build" rule the helper uses for its own contract check.
  var LIMIT = opts.limit || 3;
  // WHY 3 s: a keyboard Steam opens for a focused text box shows its keys well inside that.
  var FOCUS_MS = opts.focus_ms || 3000;
  var TEXTY = { text: 1, search: 1, email: 1, url: 1, tel: 1, number: 1, '': 1 };

  var timer = null, keysNow = false, maxLetters = 0;
  var opens = 0, shortRun = 0, noKeysRun = 0, noKeysCounted = false, focusSince = null;
  var attrIssue = '';

  function topKeys() {
    var out = [], nodes = doc.querySelectorAll(KEY_SEL);
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (n.hasAttribute(LITERAL)) continue;
      // WHY outer keys only: wide keys are two nested [data-key] nodes (M0); the inner one's attributes were never measured.
      if (n.parentElement && n.parentElement.closest(KEY_SEL)) continue;
      out.push(n);
    }
    return out;
  }
  function isLetter(n) {
    var name = n.getAttribute('data-key');
    return typeof name === 'string' && name.length === 1 && /^\p{L}$/u.test(name);
  }
  function textBoxFocused() {
    var a = doc.activeElement;
    if (!a) return false;
    var tag = String(a.tagName || '').toUpperCase();
    return tag === 'TEXTAREA' || (tag === 'INPUT' && (TEXTY[String(a.type || '')] === 1));
  }

  function stepAside(why) {
    // WHY both, and only through their public off(): each takes its own listeners, timers, strip and trail away and hands
    // a held-back touch to Steam. The frame stays, so the helper does not install a second copy.
    attrIssue = why;
    try { if (L.gestures && L.gestures.off) L.gestures.off(); } catch (e) { /* already gone */ }
    try { if (L.session && L.session.off) L.session.off(); } catch (e2) { /* already gone */ }
  }

  function tick() {
    try {
      var list = topKeys();
      if (list.length) {
        if (!keysNow) { keysNow = true; maxLetters = 0; opens++; }
        focusSince = null; noKeysCounted = false; noKeysRun = 0;
        if (!attrIssue) {
          // WHY "half or more" and not "any": one odd key (a special key without a keycode) must not switch the layer off;
          // Valve renaming or dropping an attribute takes it from (nearly) every key at once.
          for (var j = 0; j < REQUIRED.length; j++) {
            var missing = 0;
            for (var i = 0; i < list.length; i++) if (!list[i].hasAttribute(REQUIRED[j])) missing++;
            if (missing * 2 >= list.length) { stepAside('Steam keys no longer carry ' + REQUIRED[j]); return; }
          }
        }
        var letters = 0;
        for (var k = 0; k < list.length; k++) if (isLetter(list[k])) letters++;
        if (letters > maxLetters) maxLetters = letters;
        // A letters page seen at any moment of this opening clears the count at once (the next page may be a symbols page).
        if (maxLetters >= MIN_LETTERS) shortRun = 0;
      } else {
        if (keysNow) {                        // the keyboard just closed: was it a letters keyboard?
          keysNow = false;
          if (maxLetters < MIN_LETTERS) shortRun++;
        }
        if (textBoxFocused()) {
          var t = Date.now();
          if (focusSince === null) focusSince = t;
          else if (!noKeysCounted && t - focusSince >= FOCUS_MS) { noKeysCounted = true; noKeysRun++; }
        } else { focusSince = null; noKeysCounted = false; }
      }
    } catch (e) {
      // The watcher itself broke: stop watching, leave everything else exactly as it is.
      off();
    }
  }

  function issue() {
    if (attrIssue) return attrIssue;
    if (shortRun >= LIMIT) return 'the keyboard did not show ' + MIN_LETTERS + ' letter keys on its last ' + shortRun + ' openings';
    if (noKeysRun >= LIMIT) return 'a text box was focused ' + noKeysRun + ' times and no keyboard keys were found';
    return '';
  }

  function off() {
    if (timer !== null) { w.clearInterval(timer); timer = null; }
  }

  var api = {
    off: off,
    tick: tick,
    issue: issue,
    // Numbers and a sentence only; nothing typed.
    report: function () { return { issue: issue(), opens: opens, short_run: shortRun, no_keys_run: noKeysRun, max_letters: maxLetters }; }
  };
  timer = w.setInterval(tick, TICK_MS);
  return api;
}
