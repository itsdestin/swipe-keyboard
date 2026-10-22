// Gesture arbiter for Steam's on-screen keyboard (plan Section 7, milestone M4).
//
// WHAT IT DOES, in plain words. Steam's keyboard types the key under your finger when you LIFT it. To also
// allow swiping, this layer sits in front of Steam and looks at each touch first:
//   * a quick tap on a letter  -> held back for a moment, then handed to Steam exactly as if it had just
//                                 happened, so the letter types as it always did;
//   * a finger that slides     -> kept for ourselves (the path is recorded), Steam never hears about it, so no
//                                 stray letter appears;
//   * a finger that rests on a letter for 180 ms -> given to Steam at once so Steam's own accent pop-up and
//                                 timers work untouched;
//   * everything else (Shift, Backspace, Space, Enter, numbers, a second finger, anything while an accent
//     pop-up is open, anything in a password box) -> passes straight through, no delay.
//   * if anything at all goes wrong -> the layer takes its listeners off and the keyboard is plain Steam.
//
// It reads only: the [data-key] elements and their rectangles, standard pointer/touch events, and
// document.activeElement.type. It changes nothing in Steam; its only side effect is capture-phase listeners
// on the window. This file defines ONE function; the helper's stub (layer_stub.js) calls it.
function swipekbGestures(w, L, opts) {
  'use strict';
  opts = opts || {};
  var doc = w.document;
  if (!doc || typeof w.addEventListener !== 'function') return null;

  // WHY these numbers: plan Section 7 values (12 px swipe, 180 ms hold); stuck_ms is the plan's 1.5 s guard.
  var SWIPE_PX = opts.swipe_px || 12;
  var HOLD_MS = opts.hold_ms || 180;
  var STUCK_MS = opts.stuck_ms || 1500;
  var dom = (L && L.config && L.config.keyboard_dom) || {};
  var KEY_SEL = dom.key_selector || '[data-key]';
  var MIN_LETTERS = dom.min_letter_keys || 24;
  var LITERAL = 'data-key-is-literal';   // M0: accent pop-up items carry this; they are never keys of ours
  // WHY 2000: the engine accepts at most 2048 points per swipe (protocol.hpp); 2000 points is about 16 s of finger.
  var MAX_POINTS = 2000;
  // WHY optional: the typing session (session.js) plugs in here; without it this file behaves exactly as in M4.
  var H = opts.hooks || {};
  function hook(name, a, b) { if (typeof H[name] === 'function') return H[name](a, b); }

  // Only one gesture layer may exist per page: a newer install replaces the older, never stacks.
  if (w.__swipekbGestures) { try { w.__swipekbGestures.off(); } catch (e) { /* the old one is going away anyway */ } }

  var setT = w.setTimeout.bind(w), clearT = w.clearTimeout.bind(w);
  var now = function () { return w.performance && w.performance.now ? w.performance.now() : Date.now(); };

  var DOWN = { pointerdown: 1, touchstart: 1 };
  var MOVE = { pointermove: 1, touchmove: 1 };
  var UP = { pointerup: 1, touchend: 1 };
  var CANCEL = { pointercancel: 1, touchcancel: 1 };
  var TYPES = ['pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'touchstart', 'touchmove', 'touchend', 'touchcancel'];

  var state = 'on';          // 'on' | 'failed' | 'off'
  var reason = '';
  var st = null;             // the first finger, while we are tracking it
  var tail = null;           // ids of a finished swallowed finger whose second event family is still to come
  var active = {};           // every finger currently down (so a second finger is recognised)
  var activeN = 0;
  var holdTimer = null, stuckTimer = null, fault = false, strokeId = 0;
  var ours = new w.WeakSet();   // events we made ourselves: our own listeners ignore them
  var stats = { tap: 0, swipe: 0, hold: 0, second: 0, pass: 0, stuck: 0, swallowed: 0 };

  // ---- geometry ---------------------------------------------------------------------------------------
  function isLetter(name, el) {
    return typeof name === 'string' && name.length === 1 && /^\p{L}$/u.test(name) && !(el && el.hasAttribute(LITERAL));
  }
  // The keys on screen right now. WHY de-duplicated: M0 found wide keys (Backspace, Shift...) are two nested
  // [data-key] nodes, so a node that sits inside another key is skipped and the outer one is kept. WHY the
  // literal check: accent pop-up items look like keys but are not part of the layout.
  function keys() {
    var out = [], nodes = doc.querySelectorAll(KEY_SEL);
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (n.hasAttribute(LITERAL)) continue;
      if (n.parentElement && n.parentElement.closest(KEY_SEL)) continue;
      var r = n.getBoundingClientRect(), name = n.getAttribute('data-key');
      out.push({ key: name, x: r.left + r.width / 2, y: r.top + r.height / 2, w: r.width, h: r.height, letter: isLetter(name, n) });
    }
    return out;
  }
  function popupOpen() { return !!doc.querySelector('[' + LITERAL + ']'); }
  function passwordFocused() {
    // WHY the session is asked first: it takes its own strip down at this very touch when the box is a password box, instead of
    // leaving it on screen until its next timer tick.
    if (hook('inert') === true) return true;     // the session's view: a password box, or the Private chip is on
    var a = doc.activeElement;
    return !!(a && a.type === 'password');       // plan Section 8: the inert signal; checked on every touch, no round trip
  }

  // ---- reading events ---------------------------------------------------------------------------------
  function isTouchEv(e) { return e.type.charAt(0) === 't'; }
  function pt(e) {
    var t = isTouchEv(e) ? ((e.changedTouches && e.changedTouches[0]) || (e.touches && e.touches[0])) : e;
    return t ? [t.clientX, t.clientY] : [0, 0];
  }
  function fid(e) {   // which finger: 'p<pointerId>' or 't<identifier>'
    if (isTouchEv(e)) { var t = e.changedTouches && e.changedTouches[0]; return t ? 't' + t.identifier : 't?'; }
    return 'p' + e.pointerId;
  }
  function owns(e) { return !!st && (isTouchEv(e) ? st.tid !== null && fid(e) === st.tid : st.pid !== null && fid(e) === st.pid); }
  function swallow(e) {
    stats.swallowed++;
    e.stopImmediatePropagation();
    // WHY cancel the real touch-end: found in the VM. Steam itself cancels a touch-end, and that is what stops the browser
    // from following every tap with old-style mouse events (mousedown, mouseup, click). Because Steam never sees our
    // swallowed real touch-end, the browser did send those mouse events, and Steam typed each letter twice.
    if (e.type === 'touchend' && e.cancelable) e.preventDefault();
  }

  // ---- giving events to Steam -------------------------------------------------------------------------
  // WHY we rebuild both families: Steam's keyboard reacts to pointer events, but the spike showed the touch
  // events are needed for it to treat the press as a real touch; we replay whichever the finger produced.
  function mkPointer(type, s, x, y) {
    var ev = new w.PointerEvent(type, { bubbles: true, cancelable: true, pointerId: s.pidNum, pointerType: s.ptype || 'touch', isPrimary: true,
      clientX: x, clientY: y, button: 0, buttons: type === 'pointerdown' ? 1 : 0 });
    ours.add(ev); return ev;
  }
  function mkTouch(type, s, x, y) {
    var t = new w.Touch({ identifier: s.tidNum, target: s.target, clientX: x, clientY: y });
    var ending = type === 'touchend' || type === 'touchcancel';
    var ev = new w.TouchEvent(type, { bubbles: true, cancelable: true, touches: ending ? [] : [t], targetTouches: ending ? [] : [t], changedTouches: [t] });
    ours.add(ev); return ev;
  }
  function replay(s, which) {   // which: 'down' | 'up'
    if (!s.target || (s.target.isConnected === false)) return;
    if (s.sawPointer) s.target.dispatchEvent(mkPointer('pointer' + which, s, s.x0, s.y0));
    if (s.sawTouch) s.target.dispatchEvent(mkTouch(which === 'down' ? 'touchstart' : 'touchend', s, s.x0, s.y0));
  }

  // ---- timers -----------------------------------------------------------------------------------------
  function clearTimers() {
    if (holdTimer !== null) { clearT(holdTimer); holdTimer = null; }
    if (stuckTimer !== null) { clearT(stuckTimer); stuckTimer = null; }
  }
  function armStuck() {
    // WHY: if Steam or the system loses the finger-up, a swallowed touch would otherwise wait forever. After
    // STUCK_MS of silence we let go of it (nothing is typed) so the next touch is handled afresh.
    if (stuckTimer !== null) clearT(stuckTimer);
    stuckTimer = setT(function () {
      stuckTimer = null;
      guard(function () {
        if (st && st.mode !== 'pass') { stats.stuck++; st = null; tail = null; active = {}; activeN = 0; clearTimers(); }
      });
    }, STUCK_MS);
  }

  // ---- the arbiter ------------------------------------------------------------------------------------
  function promote(s, why) {   // hand the finger to Steam from now on: give it the press it has not seen yet
    clearTimers();
    s.mode = 'pass';
    stats[why]++;
    // WHY try/finally (M9): the session's hook must never be able to keep Steam from hearing a press it is owed. If the hook
    // throws, the press still goes to Steam and the error then reaches the handler, which takes the layer out.
    try { hook('uncertain'); } finally { replay(s, 'down'); }   // Steam will type this key itself (maybe an accent): the session cannot know which character
  }

  function track(e, s) {   // remember which ids belong to this finger as the two event families arrive
    if (isTouchEv(e)) { if (s.tid === null) { s.tid = fid(e); s.tidNum = e.changedTouches[0].identifier; s.sawTouch = true; } }
    else if (s.pid === null) { s.pid = fid(e); s.pidNum = e.pointerId; s.ptype = e.pointerType; s.sawPointer = true; }
  }

  function onDown(e) {
    if (passwordFocused()) return;   // inert: touch nothing, call nothing
    var id = fid(e);
    if (st && ((isTouchEv(e) && (st.tid === null || st.tid === id)) || (!isTouchEv(e) && (st.pid === null || st.pid === id)))) {
      // the same finger's other event family (pointer then touch, or the reverse)
      track(e, st);
      if (st.mode !== 'pass') swallow(e);
      return;
    }
    if (active[id]) return;
    var other = st || activeN > 0;
    active[id] = 1; activeN++;
    if (other) {
      // a second finger: it passes at once; if the first is still waiting on a letter it is released first
      if (st && st.mode === 'pending') promote(st, 'second');
      stats.pass++;
      var n2 = e.target && e.target.closest ? e.target.closest(KEY_SEL) : null;
      if (n2 && !n2.hasAttribute(LITERAL)) { var nm = n2.getAttribute('data-key'); hook('keyDown', nm); }
      return;
    }
    var node = e.target && e.target.closest ? e.target.closest(KEY_SEL) : null;
    if (!node) { delete active[id]; activeN--; return; }   // not on a key (Steam's other screens, our own strip)
    var name = node.getAttribute('data-key');
    var p = pt(e);
    var s = { mode: 'pass', pid: null, tid: null, pidNum: 99, tidNum: 99, sawPointer: false, sawTouch: false, ptype: 'touch',
      target: e.target, x0: p[0], y0: p[1], t0: now(), pts: [], upSeen: false, key: name, keyList: null };
    track(e, s);
    st = s;
    var letters = 0, list = keys();
    for (var i = 0; i < list.length; i++) if (list[i].letter) letters++;
    // non-letters, a pop-up on screen, or a layout that is not letters (numbers/symbols/other scripts): straight through
    if (!isLetter(name, node) || popupOpen() || letters < MIN_LETTERS) {
      stats.pass++;
      // the session keeps a memory of what was typed: a plain non-letter key is known, anything else makes it uncertain
      if (!isLetter(name, node) && !popupOpen() && letters >= MIN_LETTERS) hook('keyDown', name); else hook('uncertain');
      return;
    }
    s.mode = 'pending';
    s.keyList = list.filter(function (k) { return k.letter; });
    s.pts.push([p[0], p[1], 0]);
    swallow(e);
    holdTimer = setT(function () {
      holdTimer = null;
      guard(function () { if (st === s && s.mode === 'pending') promote(s, 'hold'); });
    }, HOLD_MS);
    armStuck();
  }

  function onMove(e) {
    if (!owns(e) || st.mode === 'pass') return;
    var p = pt(e);
    // record from one family only, or every point would be there twice
    if ((st.sawPointer && !isTouchEv(e)) || (!st.sawPointer && isTouchEv(e))) {
      if (st.pts.length < MAX_POINTS) st.pts.push([Math.round(p[0] * 10) / 10, Math.round(p[1] * 10) / 10, Math.round(now() - st.t0)]);
    }
    if (st.mode === 'pending' && Math.sqrt((p[0] - st.x0) * (p[0] - st.x0) + (p[1] - st.y0) * (p[1] - st.y0)) > SWIPE_PX) {
      st.mode = 'swipe';
      if (holdTimer !== null) { clearT(holdTimer); holdTimer = null; }
      hook('swipeStart', st.pts);
    } else if (st.mode === 'swipe') hook('swipePoint', p[0], p[1]);
    armStuck();
    swallow(e);
  }

  function finish(e, s) {   // the finger of a swallowed touch lifted
    clearTimers();
    // the other family's last event is still to come: swallow it too
    tail = { id: isTouchEv(e) ? s.pid : s.tid, until: now() + 600 };
    if (tail.id === null) tail = null;
    st = null;
    if (s.mode === 'pending') {
      stats.tap++;
      // WHY try/finally (M9): a throwing hook must not swallow the tap. The finger is already lifted and the layer is about to
      // take itself out, so nothing else would ever give Steam this letter.
      try { hook('keyTyped', s.key); }   // before the replay, so the session still sees the Shift state this letter was typed under
      finally {
        replay(s, 'down');   // WHY at the release itself: Steam types on release, so the letter lands when it always did
        replay(s, 'up');
      }
    } else if (s.mode === 'swipe') {
      stats.swipe++;
      hook('swipeEnd');
      emit(s);
    }
  }

  function onUp(e) {
    var id = fid(e);
    if (!st) {
      if (tail && tail.id === id && now() < tail.until) { tail = null; swallow(e); }
      delete active[id]; recount();
      return;
    }
    if (!owns(e)) {   // a second finger lifting
      if (active[id]) { delete active[id]; recount(); }
      return;
    }
    if (st.mode === 'pass') {
      // Steam is following this finger itself. We are done with it once its first "up" arrives.
      hook('keyUp', st.key);
      st = null; delete active[id]; recount();
      return;
    }
    swallow(e);
    // WHY wait for the pointer family when there is one: the press is over when the pointer says so; the touch
    // event comes right after and is swallowed through `tail`.
    var primary = st.sawPointer ? !isTouchEv(e) : isTouchEv(e);
    if (!primary) return;
    delete active[st.pid]; delete active[st.tid]; delete active[id]; recount();
    finish(e, st);
  }

  function onCancel(e) {
    if (!owns(e)) { delete active[fid(e)]; recount(); return; }
    if (st.mode === 'pass') { st = null; delete active[fid(e)]; recount(); return; }
    swallow(e);   // a cancelled touch types nothing
    if (st.mode === 'swipe') hook('swipeEnd');
    var s = st; clearTimers();
    tail = { id: isTouchEv(e) ? s.pid : s.tid, until: now() + 600 }; if (tail.id === null) tail = null;
    st = null; active = {}; activeN = 0;
  }
  function recount() { activeN = 0; for (var k in active) if (active[k]) activeN++; }

  // ---- recording a swipe ------------------------------------------------------------------------------
  function emit(s) {
    var msg = { v: 1, type: 'stroke', id: ++strokeId, trace: s.pts, vw: w.innerWidth, vh: w.innerHeight,
      keys: s.keyList.map(function (k) { return { k: k.key, x: Math.round(k.x * 10) / 10, y: Math.round(k.y * 10) / 10, w: Math.round(k.w), h: Math.round(k.h) }; }) };
    L.lastStroke = msg;   // kept in page memory only; the next stroke replaces it
    if (typeof H.stroke === 'function') { H.stroke(msg); return; }   // the session adds context and policy, then sends it
    try { if (typeof w.__swipekb === 'function') w.__swipekb(JSON.stringify(msg)); } catch (err) { /* helper gone: the swipe just does nothing */ }
  }

  // ---- safety -----------------------------------------------------------------------------------------
  function handler(e) {
    if (state !== 'on' || ours.has(e)) return;
    try {
      if (fault) { fault = false; throw new Error('injected fault'); }
      var t = e.type;
      if (DOWN[t]) onDown(e);
      else if (!st && !tail && activeN === 0) return;
      else if (MOVE[t]) onMove(e);
      else if (UP[t]) onUp(e);
      else if (CANCEL[t]) onCancel(e);
    } catch (err) { fail(err); }
  }
  function guard(fn) { try { fn(); } catch (err) { fail(err); } }

  function unhook() {
    for (var i = 0; i < TYPES.length; i++) w.removeEventListener(TYPES[i], handler, true);
    clearTimers();
  }
  function fail(err) {
    if (state !== 'on') return;
    state = 'failed'; reason = String((err && err.message) || err);
    unhook();
    var s = st; st = null; tail = null; active = {}; activeN = 0;
    // WHY: a tap that was being held back must not be lost. A finger still waiting on a letter gets its press
    // now (the real lift will follow and Steam types it); one already lifted gets the whole tap.
    try { if (s && s.mode === 'pending') { replay(s, 'down'); if (s.upSeen) replay(s, 'up'); } } catch (e2) { /* nothing more to do */ }
    try { doc.documentElement.setAttribute('data-swipekb-gestures', 'failed'); } catch (e3) { /* cosmetic */ }
    if (w.__swipekbGestures === api) delete w.__swipekbGestures;
  }
  function off() {
    if (state === 'on') {
      state = 'off'; unhook();
      var s = st; st = null; tail = null;
      try { if (s && s.mode === 'pending') replay(s, 'down'); } catch (e) { /* nothing more to do */ }
    }
    try { doc.documentElement.removeAttribute('data-swipekb-gestures'); } catch (e2) { /* cosmetic */ }
    if (w.__swipekbGestures === api) delete w.__swipekbGestures;
  }

  // A tap made by the layer itself (the session uses it to switch Steam's Shift latch off after a swiped word). Marked as ours,
  // so the arbiter ignores it and Steam sees an ordinary tap on that key.
  function tapKey(name) {
    var list = doc.querySelectorAll(KEY_SEL);
    for (var i = 0; i < list.length; i++) {
      var n = list[i];
      if (n.getAttribute('data-key') !== name || n.hasAttribute(LITERAL)) continue;
      if (n.parentElement && n.parentElement.closest(KEY_SEL)) continue;
      var r = n.getBoundingClientRect();
      var s = { target: n, x0: r.left + r.width / 2, y0: r.top + r.height / 2, pidNum: 98, tidNum: 98, ptype: 'touch', sawPointer: true, sawTouch: true };
      replay(s, 'down'); replay(s, 'up');
      return true;
    }
    return false;
  }

  var api = {
    tapKey: tapKey,
    off: off,
    state: function () { return state; },
    reason: function () { return reason; },
    keys: keys,
    stats: function () { var o = {}; for (var k in stats) o[k] = stats[k]; return o; },
    injectFault: function () { fault = true; }   // test hook: the next touch event throws inside the handler
  };
  for (var i = 0; i < TYPES.length; i++) w.addEventListener(TYPES[i], handler, true);
  w.__swipekbGestures = api;
  try { doc.documentElement.setAttribute('data-swipekb-gestures', 'on'); } catch (e) { /* cosmetic */ }
  return api;
}
