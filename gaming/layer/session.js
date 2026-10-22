// Typing session for Steam's on-screen keyboard (plan Sections 7 and 8, milestone M5).
//
// WHAT IT DOES, in plain words. The gesture arbiter (gesture.js) tells us what the fingers did. This file decides what
// that MEANS and shows it:
//   * which kind of text box is this?  A password box -> we do nothing at all. A box Steam names (text, search, ...) ->
//     swipe and suggestions. Anything else (the keyboard opened over a game or app, where Steam tells us nothing) ->
//     swipe and suggestions too, but nothing is remembered anywhere ("unknown mode").
//   * a swipe ends -> we send the stroke to the helper; it types the best word plus a space (through Steam's own text
//     call) and sends back three candidates, which we show on a strip just above Steam's top row of keys.
//   * tapping a candidate swaps the word we just typed for that one (the helper does the typing).
//   * a small "Private" chip on the strip turns swipe and suggestions off for this text box with one tap.
//   * a thin trail follows the finger while swiping.
// What we remember: only the text typed since the keyboard opened, in this page's memory, so the engine can use the
// previous words as context. It is wiped when the keyboard closes, the text box changes, or Enter/arrows/odd keys make it
// uncertain. Nothing is written to disk by this file, ever; the helper and the engine write nothing either.
// If anything throws, everything of ours is removed and Steam's keyboard is left exactly as it is.
function swipekbSession(w, L, opts) {
  'use strict';
  opts = opts || {};
  var doc = w.document;
  if (!doc || !doc.body) return null;
  var dom = (L && L.config && L.config.keyboard_dom) || {};
  var KEY_SEL = dom.key_selector || '[data-key]';
  var LITERAL = 'data-key-is-literal';
  var PLAIN = { text: 1, search: 1, email: 1, url: 1, tel: 1, number: 1 };   // plan Section 8: the field kinds Steam names
  var TICK_MS = opts.tick_ms || 250;
  var TRAIL_MS = opts.trail_ms || 300;      // plan Section 7: the trail is cleared 300 ms after the stroke ends
  var CTX_MAX = { plain: 20, unknown: 12 }; // same limits as the engine daemon (protocol.hpp)
  var BUF_MAX = 400;
  var LAYOUT = opts.steam_layout || 'qwerty';
  var setT = w.setTimeout.bind(w), clearT = w.clearTimeout.bind(w);

  var state = 'on';
  var buf = '';                    // text typed since the keyboard opened (page memory only; see the header)
  var privateOn = false, privateWhy = '';   // '' | 'user' | 'secret'
  var field = null, kbOpen = false, sentAny = false;
  var cands = null;                // {req, list:[{id,word}], len, busy} while the strip offers a replacement
  var pending = {};                // messages sent and not yet answered
  var keySeq = 0, pickSeq = 0;
  var shift = 'none';              // our own count of Steam's Shift latch: 'none' | 'one' | 'caps'
  var bsTimer = null, trailTimer = null, tickTimer = null, clearTimerId = null;
  var strip = null, slots = [], chip = null, canvas = null, ctx2d = null, lastPt = null;

  // ---- what kind of box is this -----------------------------------------------------------------------------
  function fieldKind() {
    var a = doc.activeElement, t = a && a.type;
    if (t === 'password') return 'inert';   // WHY exactly this signal: plan Section 8, found in the spike; no round trip
    if (typeof t === 'string' && PLAIN[t.toLowerCase()]) return 'plain';
    return 'unknown';
  }
  function policy() {
    var k = fieldKind();
    if (k === 'inert') return 'inert';
    return privateOn ? 'private' : k;
  }
  // Called on every touch and every tick. A different text box is a new typing session: forget everything.
  function checkField() {
    var a = doc.activeElement;
    if (a !== field) { field = a; reset(true); }
  }
  function reset(sayClosed) {
    buf = ''; cands = null; pending = {}; shift = 'none'; privateOn = false; privateWhy = '';
    if (bsTimer !== null) { clearT(bsTimer); bsTimer = null; }
    if (sayClosed && sentAny) { sentAny = false; send({ v: 1, type: 'closed' }); }   // lets the helper hang up on the engine
    render();
  }

  function send(obj) {
    if (typeof w.__swipekb !== 'function') return false;
    try { w.__swipekb(JSON.stringify(obj)); return true; } catch (e) { return false; }   // helper gone: nothing happens
  }

  // ---- the keyboard on screen -------------------------------------------------------------------------------
  function keyNodes() {
    var out = [], nodes = doc.querySelectorAll(KEY_SEL);
    for (var i = 0; i < nodes.length; i++) {
      var n = nodes[i];
      if (n.hasAttribute(LITERAL)) continue;
      if (n.parentElement && n.parentElement.closest(KEY_SEL)) continue;
      var r = n.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) out.push({ n: n, r: r });
    }
    return out;
  }
  function kbRect(list) {
    if (!list.length) return null;
    var l = 1e9, t = 1e9, r = -1e9, b = -1e9, hs = [];
    for (var i = 0; i < list.length; i++) {
      var q = list[i].r;
      l = Math.min(l, q.left); t = Math.min(t, q.top); r = Math.max(r, q.right !== undefined ? q.right : q.left + q.width);
      b = Math.max(b, q.bottom !== undefined ? q.bottom : q.top + q.height); hs.push(q.height);
    }
    hs.sort(function (a, c) { return a - c; });
    return { left: l, top: t, right: r, bottom: b, keyH: hs[Math.floor(hs.length / 2)] };
  }
  // True when the letter keys show capitals (Steam's only Shift signal: the labels, plan Section 7).
  function labelsUpper(list) {
    for (var i = 0; i < list.length; i++) {
      var name = list[i].n.getAttribute('data-key');
      if (typeof name === 'string' && name.length === 1 && /^\p{L}$/u.test(name)) {
        var t = String(list[i].n.textContent || '').trim();
        return t.length > 0 && t !== t.toLowerCase();
      }
    }
    return false;
  }

  // ---- the shadow buffer (page memory only) -----------------------------------------------------------------
  function append(s) { buf += s; if (buf.length > BUF_MAX) buf = buf.slice(buf.length - BUF_MAX); }
  function lastRun() { var m = /\S*$/.exec(buf); return m ? m[0] : ''; }
  // Mirror of the engine's looksLikeSecret (protocol.cpp): 8+ characters with three kinds, or 16+ with no vowels.
  function looksLikeSecret(s) {
    if (s.length >= 8) {
      var k = (/[a-z]/.test(s) ? 1 : 0) + (/[A-Z]/.test(s) ? 1 : 0) + (/[0-9]/.test(s) ? 1 : 0) + (/[^A-Za-z0-9]/.test(s) ? 1 : 0);
      if (k >= 3) return true;
    }
    return s.length >= 16 && !/[aeiouAEIOU]/.test(s);
  }
  // WHY (plan Section 8): over a game Steam gives no field type, so a typed run that looks like a password makes us step
  // aside for the rest of this keyboard session AND wipe it from memory; the chip shows why and can undo it.
  function secretCheck() {
    if (privateOn || !looksLikeSecret(lastRun())) return;
    buf = ''; cands = null;
    goPrivate('secret');
  }
  function contextWords(kind) {
    var toks = buf.split(/\s+/), words = [];
    if (buf.length && !/\s$/.test(buf)) toks.pop();   // the word still being typed is not context
    for (var i = 0; i < toks.length; i++) {
      var t = toks[i].replace(/^[^\p{L}]+|[^\p{L}]+$/gu, '').toLowerCase();
      if (t && t.length <= 60 && /^[\p{L}'’-]+$/u.test(t)) words.push(t);
    }
    return words.slice(-CTX_MAX[kind]);
  }

  // ---- Shift: capitals for a swiped word --------------------------------------------------------------------
  // Steam's text call ignores the Shift latch and leaves it armed, so after a swipe the next tapped key would come out
  // capital. We therefore (1) tell the helper to capitalise the word itself and (2) switch a one-shot latch off again with
  // Shift taps. We cannot tell a one-shot from Caps Lock by looking (both show capitals), so we count Shift presses ourselves.
  function capsFor(list) {
    if (!labelsUpper(list)) { shift = 'none'; return 'none'; }
    if (shift === 'caps') return 'all';
    shift = 'one';          // capitals we did not cause (e.g. Steam capitalising a fresh sentence): treat as one-shot
    return 'first';
  }
  // WHY a loop that looks: Steam redraws the labels a moment after a tap, so we tap, wait, look, and stop as soon as the
  // letters are small again (a one-shot needs two taps, an unexpected Caps Lock one). At most three, then give up.
  function clearLatch() {
    var n = 0;
    (function step() {
      clearTimerId = null;
      if (state !== 'on' || !L.gestures || !L.gestures.tapKey) return;
      var list = keyNodes();
      if (!labelsUpper(list)) { shift = 'none'; return; }
      if (n++ >= 3) return;
      L.gestures.tapKey('Shift');
      clearTimerId = setT(step, 90);
    })();
  }

  // ---- results coming back from the helper ------------------------------------------------------------------
  function result(r) {
    guard(function () {
      if (state !== 'on' || !r || r.v !== 1) return;
      var pend = pending[r.id]; delete pending[r.id];
      if (!pend) return;
      if (policy() === 'inert' || policy() === 'private') return;   // the box changed while the helper was working
      if (r.error || !r.committed || !r.cands || typeof r.typed !== 'string') { if (cands) cands.busy = false; return; }
      // WHY the sequence number: if another key was pressed while the helper worked, we no longer know what the box
      // holds after the word the helper typed, so the memory is wiped and the strip offers nothing.
      var same = pend.seq === keySeq;
      if (pend.kind === 'pick') buf = same ? buf.slice(0, Math.max(0, buf.length - pend.prevLen)) : '';
      else if (!same) buf = '';
      if (same) {
        append(r.typed);
        cands = { req: r.req, list: r.cands.slice(0, 3), len: r.typed.length, busy: false };
      } else cands = null;
      if (pend.kind === 'stroke' && pend.caps === 'first') clearLatch();
      secretCheck();
      render();
    });
  }

  // ---- hooks the gesture arbiter calls ----------------------------------------------------------------------
  var hooks = {
    // Asked on every touch: should the arbiter step aside completely?
    inert: function () {
      var p; try { checkField(); p = policy(); } catch (e) { fail(e); return false; }
      if (p === 'inert') removeStrip();
      return p === 'inert' || p === 'private';
    },
    keyDown: function (name) { guard(function () { keyEvent(name, 'down'); }); },
    keyUp: function (name) { guard(function () { if (name === 'Backspace' && bsTimer !== null) { clearT(bsTimer); bsTimer = null; } }); },
    keyTyped: function (name) { guard(function () { keyEvent(name, 'tap'); }); },
    uncertain: function () { guard(function () { buf = ''; cands = null; keySeq++; shift = shift === 'caps' ? 'caps' : 'none'; render(); }); },
    swipeStart: function (pts) { guard(function () { trailBegin(pts); }); },
    swipePoint: function (x, y) { guard(function () { trailPoint(x, y); }); },
    swipeEnd: function () { guard(trailEnd); },
    stroke: function (msg) { guard(function () { onStroke(msg); }); }
  };

  function keyEvent(name, how) {
    keySeq++; cands = null;
    var list = null;
    if (how === 'tap') {
      list = keyNodes();
      append(labelsUpper(list) ? String(name).toUpperCase() : name);
      if (shift === 'one') shift = 'none';   // Steam clears a one-shot latch with the key it capitalised
    } else if (/^space$/i.test(name)) append(' ');
    else if (name === 'Backspace') {
      buf = buf.slice(0, -1);
      // WHY: holding Backspace repeats it; we cannot count the repeats, so a long press makes the memory uncertain.
      if (bsTimer !== null) clearT(bsTimer);
      bsTimer = setT(function () { bsTimer = null; guard(function () { buf = ''; keySeq++; render(); }); }, 400);
    } else if (/^shift$/i.test(name)) {
      shift = shift === 'none' ? 'one' : shift === 'one' ? 'caps' : 'none';
    } else if (typeof name === 'string' && name.length === 1) append(name);   // a digit or symbol
    else buf = '';                                                            // Enter, arrows, emoji, layout keys...
    secretCheck();
    render();
  }

  function onStroke(msg) {
    var p = policy();
    if (p !== 'plain' && p !== 'unknown') return;
    var list = keyNodes();
    var out = { v: 1, type: 'stroke', id: msg.id, trace: msg.trace, vw: msg.vw, vh: msg.vh, keys: msg.keys,
      ctx: contextWords(p), mode: p, layout: LAYOUT, caps: capsFor(list) };
    pending[out.id] = { kind: 'stroke', seq: keySeq, caps: out.caps };
    if (send(out)) sentAny = true; else delete pending[out.id];
  }

  function pick(i) {
    if (state !== 'on' || !cands || cands.busy || !cands.list[i]) return;
    var p = policy();
    if (p !== 'plain' && p !== 'unknown') return;
    var id = 1000000 + (++pickSeq);
    pending[id] = { kind: 'pick', seq: keySeq, prevLen: cands.len };
    cands.busy = true;
    if (send({ v: 1, type: 'pick', id: id, req: cands.req, cand: cands.list[i].id, erase: cands.len })) sentAny = true;
    else { delete pending[id]; cands.busy = false; }
  }

  function goPrivate(why) {
    privateOn = true; privateWhy = why; cands = null; pending = {}; buf = '';   // WHY wipe: Private means nothing of this box is kept
    if (sentAny) { sentAny = false; send({ v: 1, type: 'closed' }); }   // the engine forgets this connection at once
    render();
  }
  function setPrivate(on) {
    if (on) goPrivate('user');
    else { privateOn = false; privateWhy = ''; render(); }
  }

  // ---- the strip --------------------------------------------------------------------------------------------
  function el(tag, cls) { var e = doc.createElement(tag); e.setAttribute('class', cls); return e; }
  function removeStrip() {
    if (strip) { try { strip.remove(); } catch (e) { /* page is going away */ } strip = null; slots = []; chip = null; }
  }
  // WHY these handlers: a touch on our strip must not reach Steam (it could move focus out of the text box and close the
  // keyboard), and the browser must not follow our tap with old-style mouse events.
  function ownTouch(e) { if (e.cancelable) e.preventDefault(); e.stopPropagation(); }
  function ensureStrip() {
    if (strip) return;
    strip = el('div', 'swipekb-strip');
    var s = strip.style;
    s.position = 'fixed'; s.zIndex = '2147483646'; s.display = 'flex'; s.alignItems = 'stretch'; s.boxSizing = 'border-box';
    s.background = 'rgba(18,22,30,0.94)'; s.color = '#f2f5fa'; s.fontFamily = 'inherit'; s.userSelect = 'none'; s.touchAction = 'none';
    s.borderRadius = '10px 10px 0 0'; s.gap = '6px'; s.padding = '4px';
    chip = el('div', 'swipekb-chip'); chip.setAttribute('data-swipekb', 'private');
    chip.textContent = 'Private';
    chip.style.cssText = 'flex:0 0 auto;display:flex;align-items:center;padding:0 14px;border-radius:8px;border:1px solid rgba(255,255,255,.35);font-size:15px';
    strip.append(chip);
    for (var i = 0; i < 3; i++) {
      var sl = el('div', 'swipekb-slot'); sl.setAttribute('data-swipekb', 'slot' + i);
      sl.style.cssText = 'flex:1 1 0;min-width:0;display:flex;align-items:center;justify-content:center;border-radius:8px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;font-size:22px';
      strip.append(sl); slots.push(sl);
    }
    ['pointerdown', 'touchstart', 'mousedown', 'touchmove', 'pointermove', 'mouseup', 'click', 'pointercancel', 'touchcancel'].forEach(function (t) {
      strip.addEventListener(t, ownTouch);
    });
    strip.addEventListener('touchend', ownTouch);
    strip.addEventListener('pointerup', function (e) {
      ownTouch(e);
      guard(function () {
        var t = e.target && e.target.closest ? e.target.closest('[data-swipekb]') : null, id = t && t.getAttribute('data-swipekb');
        if (id === 'private') setPrivate(!privateOn);
        else if (id && id.indexOf('slot') === 0) pick(Number(id.slice(4)));
      });
    });
    doc.body.append(strip);
  }
  function place(list) {
    var r = kbRect(list);
    if (!r || !strip) return;
    var h = Math.max(36, Math.min(64, Math.round(r.keyH * 0.62)));
    var s = strip.style;
    // Directly above the top row of Steam's keys, as wide as the keys. Never moves Steam's elements.
    s.left = Math.round(r.left) + 'px'; s.width = Math.round(r.right - r.left) + 'px';
    s.height = h + 'px'; s.top = Math.max(0, Math.round(r.top - h - 2)) + 'px';
  }
  // WHY the order 2-1-3: the strongest candidate sits in the middle, under the thumb that swiped.
  var ORDER = [1, 0, 2];
  function render() {
    if (!strip) return;
    var p = policy();
    var priv = p === 'private';
    chip.style.background = priv ? 'rgba(90,160,255,.9)' : 'transparent';
    chip.textContent = priv ? (privateWhy === 'secret' ? 'Private (looks like a password)' : 'Private: on') : 'Private';
    chip.setAttribute('aria-pressed', priv ? 'true' : 'false');
    for (var i = 0; i < 3; i++) {
      var c = !priv && cands && cands.list[ORDER[i]];
      slots[i].textContent = c ? c.word : '';
      slots[i].style.visibility = priv ? 'hidden' : 'visible';
      slots[i].style.background = c ? (i === 1 ? 'rgba(255,255,255,.16)' : 'rgba(255,255,255,.08)') : 'transparent';
      slots[i].setAttribute('data-swipekb', 'slot' + ORDER[i]);
    }
  }

  // ---- the trail --------------------------------------------------------------------------------------------
  function trailBegin(pts) {
    if (trailTimer !== null) { clearT(trailTimer); trailTimer = null; }
    if (!canvas) {
      canvas = el('canvas', 'swipekb-trail');
      canvas.width = w.innerWidth; canvas.height = w.innerHeight;
      var s = canvas.style;
      s.position = 'fixed'; s.left = '0'; s.top = '0'; s.width = '100%'; s.height = '100%'; s.pointerEvents = 'none'; s.zIndex = '2147483645';
      ctx2d = canvas.getContext ? canvas.getContext('2d') : null;
      if (!ctx2d) { canvas = null; return; }
      doc.body.append(canvas);
    }
    ctx2d.clearRect(0, 0, canvas.width, canvas.height);
    ctx2d.lineWidth = 8; ctx2d.lineCap = 'round'; ctx2d.lineJoin = 'round'; ctx2d.strokeStyle = 'rgba(110,190,255,0.85)';
    lastPt = null;
    for (var i = 0; i < pts.length; i++) trailPoint(pts[i][0], pts[i][1]);
  }
  function trailPoint(x, y) {
    if (!ctx2d) return;
    if (lastPt) { ctx2d.beginPath(); ctx2d.moveTo(lastPt[0], lastPt[1]); ctx2d.lineTo(x, y); ctx2d.stroke(); }
    lastPt = [x, y];
  }
  function trailEnd() {
    if (trailTimer !== null) clearT(trailTimer);
    trailTimer = setT(trailGone, TRAIL_MS);
  }
  function trailGone() {
    trailTimer = null;
    if (canvas) { try { canvas.remove(); } catch (e) { /* page is going away */ } }
    canvas = null; ctx2d = null; lastPt = null;
  }

  // ---- watching the keyboard come and go --------------------------------------------------------------------
  function tick() {
    guard(function () {
      if (state !== 'on') return;
      if (L.gestures && L.gestures.state() !== 'on') { off(); return; }   // the arbiter failed: step aside completely
      var list = keyNodes(), r = kbRect(list);
      if (!r) {
        if (kbOpen) { kbOpen = false; field = null; reset(true); removeStrip(); trailGone(); }
        return;
      }
      kbOpen = true;
      checkField();
      if (policy() === 'inert') { removeStrip(); return; }
      ensureStrip(); place(list); render();
    });
  }

  // ---- safety -----------------------------------------------------------------------------------------------
  function guard(fn) { try { fn(); } catch (e) { fail(e); } }
  function fail(err) {
    if (state !== 'on') return;
    state = 'failed';
    off();
    // WHY: a session that broke takes the arbiter with it, so Steam's keyboard is plain stock again.
    try { if (L.gestures && L.gestures.off) L.gestures.off(); } catch (e2) { /* nothing more to do */ }
    try { L.sessionError = String((err && err.message) || err); } catch (e3) { /* cosmetic */ }
  }
  function off() {
    if (state === 'on') state = 'off';
    if (tickTimer !== null) { w.clearInterval(tickTimer); tickTimer = null; }
    if (bsTimer !== null) { clearT(bsTimer); bsTimer = null; }
    if (trailTimer !== null) { clearT(trailTimer); trailTimer = null; }
    if (clearTimerId !== null) { clearT(clearTimerId); clearTimerId = null; }
    removeStrip(); trailGone();
    buf = ''; cands = null; pending = {};
    if (sentAny) { sentAny = false; send({ v: 1, type: 'closed' }); }
  }

  var api = {
    hooks: hooks,
    result: result,
    off: off,
    state: function () { return state; },
    // For tests and the VM rig: sizes and flags only, never the text.
    debug: function () { return { bufferLength: buf.length, words: contextWords('plain').length, policy: policy(), shift: shift, candidates: cands ? cands.list.length : 0, stripShown: !!strip, kbOpen: kbOpen }; }
  };
  tickTimer = w.setInterval(tick, TICK_MS);
  return api;
}
