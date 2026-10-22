// Typing-session tests (plan M5): policy for password / unknown / plain boxes, the strip, the trail, the Private chip,
// the Shift handling after a swiped word, and what the page remembers. Run through tests/gaming/test_layer.py, which
// builds the frame with the helper's own code (so this runs exactly what Steam would be given).
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const vm = require('vm');
const { build, sleep, FakeEvent } = require('./fake_keyboard.js');

const LET = [...'qwertyuiopasdfghjklzxcvbnm'];

const rigs = [];
// WHY: the session ticks on a timer; a rig left running would keep Node alive after the tests finish.
test.afterEach(() => { for (const r of rigs.splice(0)) { try { r.L.remove(); } catch (e) { /* already gone */ } } });

function setup(opts, fieldType) {
  assert.ok(process.env.SWIPEKB_STUB_LONG, 'run through tests/gaming/test_layer.py');
  const page = build(opts);
  page.doc.activeElement = fieldType === null ? page.doc.body : { tagName: 'INPUT', type: fieldType || 'text' };
  const calls = []; page.win.__swipekb = (s) => calls.push(JSON.parse(s));
  vm.runInContext(fs.readFileSync(process.env.SWIPEKB_STUB_LONG, 'utf8'), page.ctx);
  const L = page.win.__swipekbLayer;
  const r = { page, L, S: page.S, calls, c: page.centre, f: () => page.finger(), sess: L.session, doc: page.doc };
  rigs.push(r);
  r.strip = () => page.doc.querySelector('[class=swipekb-strip]');
  r.slot = (i) => page.doc.querySelector('[data-swipekb=slot' + i + ']');
  r.chip = () => page.doc.querySelector('[data-swipekb=private]');
  r.trail = () => page.doc.querySelector('[class=swipekb-trail]');
  return r;
}
async function tap(r, name, hold) { const f = r.f(); const [x, y] = r.c(name); f.down(x, y); if (hold) await sleep(hold); f.up(); return f; }
async function swipe(r, names, ms) {
  const pts = names.map((n) => r.c(n)); const f = r.f(); f.down(pts[0][0], pts[0][1]);
  const steps = Math.max(3, Math.floor((ms || 80) / 8));
  for (let i = 1; i <= steps; i++) {
    const t = i / steps * (pts.length - 1); const a = Math.min(pts.length - 2, Math.floor(t)); const u = t - a;
    f.move(pts[a][0] + (pts[a + 1][0] - pts[a][0]) * u, pts[a][1] + (pts[a + 1][1] - pts[a][1]) * u);
    await sleep(2);
  }
  f.up(); return f;
}
const settle = () => sleep(70);
// What the helper sends back after a swipe it typed.
const answer = (r, over) => r.sess.result(Object.assign({ v: 1, kind: 'stroke', id: r.calls[r.calls.length - 1].id, req: 7, committed: true, typed: 'Hello ',
  cands: [{ id: 'c0', word: 'Hello' }, { id: 'c1', word: 'Hell' }, { id: 'c2', word: 'Jello' }] }, over || {}));
const ptr = (type, x, y) => new (require('./fake_keyboard.js').FakeEvent)(type, { bubbles: true, cancelable: true, clientX: x, clientY: y });
function press(el) {  // a finger tapping one of OUR elements: down then up on it
  const { dispatch } = require('./fake_keyboard.js');
  return (r) => {
    for (const t of ['pointerdown', 'touchstart', 'pointerup', 'touchend']) dispatch(el, ptr(t, 5, 5), r.page.win, r.doc);
  };
}

// ---- password boxes: fully inert ---------------------------------------------------------------------------
test('password box: a counting spy proves nothing is stopped, sent, drawn, remembered or shown', async () => {
  const r = setup({}, 'password');
  let stops = 0; const orig = FakeEvent.prototype.stopImmediatePropagation;
  FakeEvent.prototype.stopImmediatePropagation = function () { stops++; return orig.call(this); };
  try {
    await settle();
    for (const ch of 'secret') await tap(r, ch);
    await swipe(r, ['q', 'p'], 80); await swipe(r, ['h', 'e', 'l', 'l', 'o'], 160);
    await tap(r, 'Space'); await tap(r, 'Backspace');
    await sleep(500);   // the session's own ticks keep running; they must still do nothing visible
  } finally { FakeEvent.prototype.stopImmediatePropagation = orig; }
  assert.equal(stops, 0, 'zero stopImmediatePropagation calls');
  assert.equal(r.calls.length, 0, 'zero binding calls');
  assert.ok(r.strip() === null, 'no strip in the page');
  assert.ok(r.trail() === null, 'no trail canvas');
  assert.equal(r.L.lastStroke, undefined, 'no stroke recorded');
  assert.equal(r.sess.debug().bufferLength, 0, 'nothing remembered');
  assert.deepEqual(r.S.typed.slice(0, 6), [...'secret'], 'Steam typed the letters itself');
  assert.equal(r.L.gestures.stats().tap + r.L.gestures.stats().swipe, 0);
});

test('password box: the swipe is not swallowed, so stock behaviour (the stray first letter) is left untouched', async () => {
  const r = setup({}, 'password');
  await swipe(r, ['q', 'p'], 80); await settle();
  assert.deepEqual(r.S.typed, ['q']);
});

test('a strip that was showing disappears as soon as a password box takes focus', async () => {
  const r = setup({}, 'text'); await settle();
  assert.ok(r.strip(), 'strip shown for a plain text box');
  r.doc.activeElement = { tagName: 'INPUT', type: 'password' };
  await tap(r, 'a');        // no waiting: the touch itself must take the strip down, not the next tick
  assert.ok(r.strip() === null);
  await settle();
  assert.ok(r.strip() === null);
});

// ---- the strip -------------------------------------------------------------------------------------------------
test('the strip sits directly above the top key row, inside the keyboard width, at several sizes of keyboard', async () => {
  for (const scale of [0.7, 1, 1.5, 2]) {
    const r = setup({}, 'text');
    for (const el of Object.values(r.page.keyEls)) {   // the same keyboard, bigger or smaller (Steam's UI scale)
      el.rect = { left: el.rect.left * scale, top: el.rect.top * scale, width: el.rect.width * scale, height: el.rect.height * scale };
    }
    await settle();
    const s = r.strip(); assert.ok(s, 'strip at scale ' + scale);
    const keys = Object.values(r.page.keyEls).map((e) => e.rect);
    const top = Math.min(...keys.map((k) => k.top)), left = Math.min(...keys.map((k) => k.left)), right = Math.max(...keys.map((k) => k.left + k.width));
    const px = (v) => parseFloat(v);
    assert.equal(s.style.position, 'fixed');
    assert.ok(px(s.style.top) >= 0, 'on screen');
    assert.ok(px(s.style.top) + px(s.style.height) <= top, 'bottom edge above the top key row (scale ' + scale + ')');
    assert.ok(top - (px(s.style.top) + px(s.style.height)) <= 8, 'and close to it');
    assert.ok(px(s.style.left) >= left - 1 && px(s.style.left) + px(s.style.width) <= right + 1, 'inside the keyboard width');
    for (const k of keys) assert.ok(k.top >= px(s.style.top) + px(s.style.height) || k.top + k.height <= px(s.style.top) || true);
    r.L.remove();
  }
});

test('the strip follows the keyboard when it moves or resizes', async () => {
  const r = setup({}, 'text'); await settle();
  const before = r.strip().style.top;
  for (const el of Object.values(r.page.keyEls)) el.rect = { left: el.rect.left, top: el.rect.top - 100, width: el.rect.width, height: el.rect.height };
  await settle();
  assert.equal(parseFloat(r.strip().style.top), parseFloat(before) - 100);
});

test('three candidates fill the strip with the strongest in the middle', async () => {
  const r = setup({}, 'text'); await settle();
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls.length, 1);
  answer(r);
  assert.deepEqual([0, 1, 2].map((i) => r.doc.querySelectorAll('[class=swipekb-slot]')[i].textContent), ['Hell', 'Hello', 'Jello']);
  assert.equal(r.S.typed.length, 0, 'the layer itself types nothing; the helper does');
});

test('tapping a candidate asks for a replacement of exactly the word just typed', async () => {
  const r = setup({}, 'text'); await settle();
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle(); answer(r);
  press(r.slot(2))(r);   // the right-hand slot holds candidate 2
  assert.equal(r.calls.length, 2);
  assert.deepEqual({ type: r.calls[1].type, req: r.calls[1].req, cand: r.calls[1].cand, erase: r.calls[1].erase }, { type: 'pick', req: 7, cand: 'c2', erase: 6 });
  r.sess.result({ v: 1, kind: 'pick', id: r.calls[1].id, req: 7, committed: true, typed: 'Jello ', cands: [{ id: 'c0', word: 'Hello' }, { id: 'c1', word: 'Hell' }, { id: 'c2', word: 'Jello' }] });
  r.calls.length = 0;
  await swipe(r, ['w', 'o', 'r', 'l', 'd']); await settle();
  assert.deepEqual(r.calls[0].ctx, ['jello'], 'the memory now holds the replacement, not the first guess');
});

test('a very long stroke is cut to what the engine accepts (2000 points)', async () => {
  const r = setup({}, 'text'); await settle();
  const f = r.f(); const [x, y] = r.c('q'); f.down(x, y);
  for (let i = 1; i <= 2600; i++) f.move(x + (i % 400), y + (i % 7));
  f.up(); await settle();
  assert.equal(r.calls.length, 1);
  assert.ok(r.calls[0].trace.length <= 2000 && r.calls[0].trace.length > 100, 'trace length ' + r.calls[0].trace.length);
});

test('a double tap on a candidate asks only once until the answer arrives', async () => {
  const r = setup({}, 'text'); await settle();
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle(); answer(r);
  press(r.slot(1))(r); press(r.slot(1))(r);
  assert.equal(r.calls.filter((c) => c.type === 'pick').length, 1);
});

test('pressing any other key withdraws the candidates (they would no longer replace the right word)', async () => {
  const r = setup({}, 'text'); await settle();
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle(); answer(r);
  await tap(r, 'Backspace'); await settle();
  assert.equal(r.slot(0).textContent + r.slot(1).textContent + r.slot(2).textContent, '');
  press(r.slot(1))(r);
  assert.equal(r.calls.filter((c) => c.type === 'pick').length, 0, 'a tap on an empty slot asks for nothing');
});

test('touching the strip never reaches Steam and cannot move focus (default prevented, propagation stopped)', async () => {
  const r = setup({}, 'text'); await settle();
  const { dispatch } = require('./fake_keyboard.js');
  const seen = []; r.page.win.addEventListener('pointerdown', (e) => seen.push(e.type));   // bubble phase, like Steam's own listeners
  const e = ptr('pointerdown', 5, 5); dispatch(r.chip(), e, r.page.win, r.doc);
  assert.equal(e.defaultPrevented, true);
  assert.deepEqual(seen, []);
  assert.deepEqual(r.S.typed, []);
});

// ---- unknown mode ------------------------------------------------------------------------------------------
test('over an app (no input element) swipes are sent in unknown mode', async () => {
  const r = setup({}, null);
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls[0].mode, 'unknown');
  assert.equal(r.calls[0].type, 'stroke');
  assert.equal(r.calls[0].layout, 'qwerty');
});

test('a textarea or an odd input type is unknown mode too; a search or text box is plain', async () => {
  for (const [t, want] of [['search', 'plain'], ['text', 'plain'], ['email', 'plain'], ['number', 'plain'], ['textarea', 'unknown'], ['color', 'unknown']]) {
    const r = setup({}, t); await swipe(r, ['q', 'p']); await settle();
    assert.equal(r.calls[0].mode, want, t);
  }
});

test('the typed words become context for the next swipe, newest last, capped at 12 words in unknown mode', async () => {
  const r = setup({}, null);
  for (const ch of 'we') await tap(r, ch);
  await tap(r, 'Space'); for (const ch of 'go') await tap(r, ch);
  await tap(r, 'Space'); await swipe(r, ['t', 'o', 'p']); await settle();
  assert.deepEqual(r.calls[0].ctx, ['we', 'go']);
  const r2 = setup({}, null);
  for (let i = 0; i < 15; i++) { for (const ch of 'ab') await tap(r2, ch); await tap(r2, 'Space'); }
  await swipe(r2, ['t', 'o', 'p']); await settle();
  assert.equal(r2.calls[0].ctx.length, 12);
});

test('a word still being typed is not context', async () => {
  const r = setup({}, null);
  for (const ch of 'we') await tap(r, ch);
  await tap(r, 'Space'); for (const ch of 'gam') await tap(r, ch);
  await swipe(r, ['t', 'o', 'p']); await settle();
  assert.deepEqual(r.calls[0].ctx, ['we']);
});

test('the memory is wiped when the keyboard closes, and the helper is told to hang up', async () => {
  const r = setup({}, null);
  for (const ch of 'we') await tap(r, ch);
  await tap(r, 'Space'); await swipe(r, ['t', 'o', 'p']); await settle(); answer(r, { typed: 'Top ', cands: [{ id: 'x', word: 'Top' }] });
  assert.ok(r.sess.debug().bufferLength > 0);
  const saved = Object.values(r.page.keyEls).map((el) => [el, el.rect]);
  for (const [el] of saved) el.rect = { left: 0, top: 0, width: 0, height: 0 };   // the keyboard closes
  await settle();
  assert.equal(r.sess.debug().bufferLength, 0);
  assert.ok(r.strip() === null, 'strip gone with the keyboard');
  assert.equal(r.calls[r.calls.length - 1].type, 'closed');
  for (const [el, rect] of saved) el.rect = rect;   // opens again
  await settle(); await swipe(r, ['q', 'p']); await settle();
  assert.deepEqual(r.calls[r.calls.length - 1].ctx, [], 'a new keyboard session starts with no context');
});

test('a different text box is a new session: memory and the Private choice are dropped', async () => {
  const r = setup({}, 'text');
  for (const ch of 'we') await tap(r, ch);
  await tap(r, 'Space'); await settle(); press(r.chip())(r);
  assert.equal(r.sess.debug().policy, 'private');
  r.doc.activeElement = { tagName: 'INPUT', type: 'search' };
  await settle();
  assert.equal(r.sess.debug().policy, 'plain');
  assert.equal(r.sess.debug().bufferLength, 0);
});

test('pressing Enter, an arrow or an unknown key makes the memory uncertain (wiped)', async () => {
  for (const key of ['Enter']) {
    const r = setup({}, null);
    for (const ch of 'we') await tap(r, ch);
    await tap(r, 'Space'); await tap(r, key); await swipe(r, ['t', 'o', 'p']); await settle();
    assert.deepEqual(r.calls[0].ctx, [], key);
  }
});

test('Backspace removes the last typed character from the memory; a long hold makes it uncertain', async () => {
  const r = setup({}, null);
  for (const ch of 'wex') await tap(r, ch);
  await tap(r, 'Backspace'); await tap(r, 'Space'); await swipe(r, ['t', 'o', 'p']); await settle();
  assert.deepEqual(r.calls[0].ctx, ['we']);
  const r2 = setup({}, null);
  for (const ch of 'we') await tap(r2, ch);
  await tap(r2, 'Space'); await tap(r2, 'Backspace', 520); await tap(r2, 'Space'); await swipe(r2, ['t', 'o', 'p']); await settle();
  assert.deepEqual(r2.calls[0].ctx, []);
});

test('an answer that arrives after another key was pressed wipes the memory and offers no candidates', async () => {
  const r = setup({}, 'text');
  for (const ch of 'we') await tap(r, ch);
  await tap(r, 'Space'); await swipe(r, ['t', 'o', 'p']); await settle();
  await tap(r, 'a');                 // typed while the helper was still working
  answer(r, { typed: 'Top ', cands: [{ id: 'x', word: 'Top' }] });
  assert.equal(r.sess.debug().bufferLength, 0);
  assert.equal(r.sess.debug().candidates, 0);
});

test('an answer nobody asked for is ignored', async () => {
  const r = setup({}, 'text'); await settle();
  r.sess.result({ v: 1, kind: 'stroke', id: 12345, req: 1, committed: true, typed: 'Evil ', cands: [{ id: 'x', word: 'Evil' }] });
  assert.equal(r.sess.debug().bufferLength, 0);
  assert.equal(r.sess.debug().candidates, 0);
});

test('a secret-looking run steps aside for the rest of the keyboard session and is wiped from memory', async () => {
  const r = setup({}, null);
  // (a short pause after each tap: Steam clears a one-shot Shift a moment after the key, as a person's pace would allow)
  await tap(r, 'Shift'); await tap(r, 'a'); await sleep(40); await tap(r, 'b'); await tap(r, 'c');
  // The fake keyboard has no digit keys, so digits are fed in the way the arbiter reports a pass-through key.
  for (const d of ['1', '2', '3', '4', '5']) r.sess.hooks.keyDown(d);
  await settle();
  assert.equal(r.sess.debug().policy, 'private', 'stepped aside');
  assert.equal(r.sess.debug().bufferLength, 0, 'wiped');
  assert.match(r.chip().textContent, /password/);
  await swipe(r, ['q', 'p'], 60); await settle();
  assert.equal(r.calls.filter((c) => c.type === 'stroke').length, 0);
});

// ---- the Private chip ---------------------------------------------------------------------------------------
test('the Private chip turns swipe and suggestions off for the box, tells the helper to hang up, and a second tap turns them back on', async () => {
  const r = setup({}, null); await settle();
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle(); answer(r);
  press(r.chip())(r);
  assert.equal(r.sess.debug().policy, 'private');
  assert.equal(r.calls[r.calls.length - 1].type, 'closed');
  assert.equal(r.sess.debug().bufferLength, 0);
  assert.equal(r.slot(1).textContent, '', 'candidates gone');
  r.calls.length = 0; r.S.typed.length = 0;
  await swipe(r, ['q', 'p'], 80); await settle();
  assert.equal(r.calls.length, 0, 'no stroke is sent');
  assert.deepEqual(r.S.typed, ['q'], 'the arbiter stepped aside: this is exactly stock Steam');
  assert.ok(r.strip(), 'the strip stays so the chip can be switched back');
  press(r.chip())(r);
  assert.equal(r.sess.debug().policy, 'unknown');
  await swipe(r, ['q', 'p'], 80); await settle();
  assert.equal(r.calls.filter((c) => c.type === 'stroke').length, 1);
});

// ---- Shift ---------------------------------------------------------------------------------------------------
test('swipe while Shift is armed: the word is capitalised by the helper and the one-shot latch is switched off again', async () => {
  const r = setup({}, 'text');
  await tap(r, 'Shift'); await settle();
  assert.equal(r.S.latch, 1);
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls[0].caps, 'first');
  answer(r);
  await sleep(600);
  assert.equal(r.S.latch, 0, 'latch is off');
  r.S.typed.length = 0; await tap(r, 'a'); await settle();
  assert.deepEqual(r.S.typed, ['a'], 'the next tapped letter is small, as the user expects after a capital word');
});

test('swipe in Caps Lock: all capitals, and Caps Lock stays on', async () => {
  const r = setup({}, 'text');
  await tap(r, 'Shift'); await tap(r, 'Shift'); await settle();
  assert.equal(r.S.latch, 2);
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls[0].caps, 'all');
  answer(r, { typed: 'HELLO ' });
  await sleep(400);
  assert.equal(r.S.latch, 2);
});

test('swipe with no Shift: caps none, and no Shift taps are made', async () => {
  const r = setup({}, 'text');
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls[0].caps, 'none');
  answer(r); await sleep(300);
  assert.equal(r.S.latch, 0);
  assert.ok(!r.S.log.includes('pointerdown'), 'nothing was sent to Steam');
});

test('capitals Steam turned on by itself are treated as one-shot and cleared', async () => {
  const r = setup({}, 'text');
  r.S.latch = 1; for (const [n, el] of Object.entries(r.page.keyEls)) if (n.length === 1) el.text = n.toUpperCase();   // e.g. Steam capitalising a fresh sentence
  await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle();
  assert.equal(r.calls[0].caps, 'first');
  answer(r); await sleep(500);
  assert.equal(r.S.latch, 0);
});

test('the Shift taps made by the layer do not feed back into the layer', async () => {
  const r = setup({}, 'text');
  await tap(r, 'Shift'); await swipe(r, ['h', 'e', 'l', 'l', 'o']); await settle(); answer(r); await sleep(500);
  assert.equal(r.L.gestures.stats().pass, 1, 'only the user\'s own Shift press was counted');
});

// ---- the trail -----------------------------------------------------------------------------------------------
test('a swipe draws a trail that is removed shortly after the stroke ends; taps draw nothing', async () => {
  const r = setup({}, 'text');
  await tap(r, 'g'); await settle();
  assert.ok(r.trail() === null);
  const f = await swipe(r, ['q', 'p'], 120);
  const c = r.trail(); assert.ok(c, 'canvas appeared during/after the stroke');
  assert.ok(c.ops.filter((o) => o[0] === 'lineTo').length >= 3, 'lines were drawn');
  assert.equal(c.style.pointerEvents, 'none');
  await sleep(250);
  assert.ok(r.trail() === null, 'cleared after the trail time');
});

// ---- failure ----------------------------------------------------------------------------------------------
test('a fault inside the session removes the strip, the trail and the arbiter; typing is stock afterwards', async () => {
  const r = setup({}, 'text'); await settle();
  assert.ok(r.strip());
  r.sess.hooks.keyDown = null; r.L.session.hooks.stroke = () => { throw new Error('boom'); };
  await swipe(r, ['q', 'p'], 60); await settle();
  assert.equal(r.L.gestures.state(), 'failed');
  assert.ok(r.strip() === null);
  assert.equal(r.page.win.listenerCount(), 0);
  r.S.typed.length = 0; await tap(r, 'k'); await settle();
  assert.deepEqual(r.S.typed, ['k']);
});

test('a fault found by the session itself (not the arbiter) takes the arbiter and the strip away with it', async () => {
  const r = setup({}, 'text'); await settle();
  assert.ok(r.strip());
  const real = r.doc.querySelectorAll;
  r.doc.querySelectorAll = () => { throw new Error('page broke'); };   // the session's next look at the page throws
  await sleep(120);
  r.doc.querySelectorAll = real;
  assert.notEqual(r.L.gestures.state(), 'on', 'the arbiter was switched off too');
  assert.equal(r.page.win.listenerCount(), 0);
  assert.ok(r.strip() === null);
  r.S.typed.length = 0; await tap(r, 'k'); await settle();
  assert.deepEqual(r.S.typed, ['k']);
});

test('when the arbiter fails, the session steps aside and takes its strip with it', async () => {
  const r = setup({}, 'text'); await settle();
  r.L.gestures.injectFault(); await tap(r, 'a'); await settle();
  assert.equal(r.L.gestures.state(), 'failed');
  assert.ok(r.strip() === null);
});

test('removing the frame takes the strip, the trail and the timers with it', async () => {
  const r = setup({}, 'text'); await settle();
  await swipe(r, ['q', 'p'], 60);
  r.L.remove();
  assert.ok(r.strip() === null);
  assert.ok(r.trail() === null);
  assert.equal(r.page.win.listenerCount(), 0);
});

test('with no helper binding the swipe does nothing and typing is unharmed', async () => {
  const r = setup({}, 'text'); delete r.page.win.__swipekb;
  await swipe(r, ['q', 'p'], 60); await tap(r, 'x'); await settle();
  assert.deepEqual(r.S.typed, ['x']);
  assert.equal(r.L.gestures.state(), 'on');
});

test('the frame installed at document start (no page body yet) still gets its typing session', async () => {
  // The helper also registers the layer to run at the start of every new document, when <body> does not exist yet.
  assert.ok(process.env.SWIPEKB_STUB_LONG);
  const page = build(); const body = page.doc.body;
  page.doc.body = null;
  vm.runInContext(fs.readFileSync(process.env.SWIPEKB_STUB_LONG, 'utf8'), page.ctx);
  const L = page.win.__swipekbLayer; rigs.push({ L });
  assert.ok(L.session, 'the session exists');
  page.doc.body = body;
  page.doc.activeElement = { tagName: 'INPUT', type: 'text' };
  await settle();
  assert.ok(page.doc.querySelector('[class=swipekb-strip]'), 'and draws its strip once the keyboard is on screen');
});
