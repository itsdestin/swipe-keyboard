// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Failure injection for the page layer (plan M9). A model of Steam's keyboard (fake_keyboard.js) is changed the way Valve could
// change the real one, and each time the same two things must hold: Steam's keyboard behaves EXACTLY as it does with no layer
// at all (compared event for event, not just letter for letter), and the layer says what it saw. Run through
// tests/gaming/test_layer.py (it builds the frame with the helper's own code).
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { build, sleep } = require('./fake_keyboard.js');

const LET = [...'qwertyuiopasdfghjklzxcvbnm'];
const rigs = [];
test.afterEach(() => { for (const r of rigs.splice(0)) { try { r.L.remove(); } catch (e) { /* already gone */ } } });

// A page with the layer in it (on=true) or without (on=false: the stock keyboard we compare against).
function setup(opts, on, fieldType) {
  assert.ok(process.env.SWIPEKB_STUB_LONG, 'run through tests/gaming/test_layer.py');
  const page = build(opts);
  page.doc.activeElement = { tagName: 'INPUT', type: fieldType || 'text' };
  const live = new Set();           // intervals started and not cleared: a removed layer must leave none
  const setI = page.win.setInterval, clrI = page.win.clearInterval;
  page.win.setInterval = (f, ms) => { const t = setI(f, ms); live.add(t); return t; };
  page.win.clearInterval = (t) => { live.delete(t); clrI(t); };
  const calls = []; page.win.__swipekb = (s) => calls.push(JSON.parse(s));
  let L = null;
  if (on !== false) { vm.runInContext(fs.readFileSync(process.env.SWIPEKB_STUB_LONG, 'utf8'), page.ctx); L = page.win.__swipekbLayer; }
  const r = { page, L, S: page.S, calls, live, c: page.centre, f: () => page.finger(), doc: page.doc, win: page.win };
  if (L) rigs.push(r); else rigs.push({ L: { remove() {} } });
  r.report = () => JSON.parse(r.L.report());
  return r;
}
// Show only `names` as keys (the rest lose data-key, as if Valve had renamed it); [] hides the keyboard entirely.
function showKeys(r, names) {
  for (const [n, el] of Object.entries(r.page.keyEls)) {
    const want = names === 'all' || names.includes(n);
    if (want && el.hasAttribute('data-k')) { el.setAttribute('data-key', el.getAttribute('data-k')); el.removeAttribute('data-k'); }
    if (!want && el.hasAttribute('data-key')) { el.setAttribute('data-k', el.getAttribute('data-key')); el.removeAttribute('data-key'); }
  }
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
const settle = () => sleep(120);
// The same finger work on two pages; what reached Steam (typed text and every event it saw) must match.
async function same(withLayer, stock, work) {
  await work(withLayer); await work(stock); await settle();
  assert.deepEqual(withLayer.S.typed, stock.S.typed, 'typed text differs from stock');
  assert.deepEqual(withLayer.S.log, stock.S.log, 'the events Steam saw differ from stock');
}
const taps = async (r) => { for (const k of ['q', 'w', 'k']) { await tap(r, k); await sleep(30); } await swipe(r, ['q', 'p'], 60); await tap(r, 'Backspace'); await tap(r, ' '); };

// ---- data-key removed -------------------------------------------------------------------------------------
test('data-key renamed away: Steam sees exactly what it sees with no layer, and the layer reports it after three focused boxes', async () => {
  const a = setup({}, true), b = setup({}, false);
  showKeys(a, []); showKeys(b, []);
  await same(a, b, taps);
  // three text boxes focused for longer than the watch time, with no keys ever appearing
  for (let i = 0; i < 3; i++) {
    a.doc.activeElement = { tagName: 'INPUT', type: 'text' }; await sleep(140);
    a.doc.activeElement = a.doc.body; await sleep(60);
  }
  const rep = a.report().keyboard;
  assert.match(rep.issue, /no keyboard keys were found/);
  assert.equal(a.L.gestures.state(), 'on', 'a count that cannot be proven wrong must not change behaviour');
});

test('one box focused with no keys is not enough to report anything', async () => {
  const a = setup({}, true); showKeys(a, []);
  await sleep(300);
  assert.equal(a.report().keyboard.issue, '');
});

test('a focused box whose keyboard does appear resets the count', async () => {
  const a = setup({}, true); showKeys(a, []);
  for (let i = 0; i < 2; i++) { a.doc.activeElement = { tagName: 'INPUT', type: 'text' }; await sleep(140); a.doc.activeElement = a.doc.body; await sleep(60); }
  a.doc.activeElement = { tagName: 'INPUT', type: 'text' }; showKeys(a, 'all'); await sleep(120);
  showKeys(a, []); a.doc.activeElement = { tagName: 'INPUT', type: 'text' }; await sleep(140);
  assert.equal(a.report().keyboard.issue, '');
});

// ---- a required attribute removed -------------------------------------------------------------------------
test('data-key-row gone from the keys: the layer switches itself off, leaves no listener, and typing is stock', async () => {
  const a = setup({}, true), b = setup({}, false);
  for (const r of [a, b]) for (const el of Object.values(r.page.keyEls)) el.removeAttribute('data-key-row');
  await sleep(150);
  assert.equal(a.L.gestures.state(), 'off');
  assert.equal(a.L.session.state(), 'off');
  assert.equal(a.win.listenerCount(), 0);
  assert.match(a.report().keyboard.issue, /no longer carry data-key-row/);
  assert.ok(a.doc.querySelector('[class=swipekb-strip]') === null, 'no strip left on screen');
  await same(a, b, taps);
});

test('a single odd key without an attribute does not switch the layer off (half or more must lack it)', async () => {
  const a = setup({}, true);
  a.page.keyEls.Enter.removeAttribute('data-keycode');
  await sleep(150);
  assert.equal(a.L.gestures.state(), 'on');
  assert.equal(a.report().keyboard.issue, '');
});

test('the inner copy of a wide key is not judged (its attributes were never measured)', async () => {
  const a = setup({ nestedWide: true }, true);
  // 14 of the 31 outer keys lose the attribute (under half) and every inner copy does too: counting the inner ones would tip it over
  LET.slice(0, 14).forEach((n) => a.page.keyEls[n].removeAttribute('data-key-row'));
  for (const el of Object.values(a.page.keyEls)) if (el.parentElement && el.parentElement.hasAttribute('data-key')) el.removeAttribute('data-key-row');
  await sleep(150);
  assert.equal(a.L.gestures.state(), 'on');
});

test('accent pop-up items are not keys of the layout: they do not count as letters and are not judged for attributes', async () => {
  const a = setup({}, true);
  const few = LET.slice(0, 18).concat(['Shift', ' ', 'Backspace', 'Enter', 'CapsLock']);
  for (let i = 0; i < 3; i++) {
    showKeys(a, few);
    const pop = a.doc.createElement('div'); a.page.kb.append(pop);
    const items = [...'éèêëēėęà'].map((ch) => { const c = a.doc.createElement('div'); pop.append(c); c.setAttribute('data-key', ch); c.setAttribute('data-key-is-literal', '1'); return c; });
    await sleep(80);
    pop.remove(); items.length = 0; showKeys(a, []); await sleep(80);
  }
  assert.match(a.report().keyboard.issue, /24 letter keys/, 'eight pop-up letters were counted as part of the layout');
  assert.equal(a.L.gestures.state(), 'on', 'pop-up items without a row were judged as keys');
});

// ---- key count dropped ------------------------------------------------------------------------------------
test('fewer than 24 letter keys: stock events only, reported after three openings, and cleared by a normal keyboard', async () => {
  const a = setup({}, true), b = setup({}, false);
  const few = LET.slice(0, 18).concat(['Shift', ' ', 'Backspace', 'Enter', 'CapsLock']);
  showKeys(a, few); showKeys(b, few);
  await same(a, b, taps);
  for (let i = 0; i < 3; i++) { showKeys(a, few); await sleep(80); showKeys(a, []); await sleep(80); }
  assert.match(a.report().keyboard.issue, /did not show 24 letter keys on its last 3 openings/);
  assert.equal(a.L.gestures.state(), 'on');
  showKeys(a, 'all'); await sleep(80);
  assert.equal(a.report().keyboard.issue, '', 'a keyboard with all its letters clears the report at once');
});

test('a symbols or number page between letters pages does not add up to a report', async () => {
  const a = setup({}, true);
  const sym = ['Shift', ' ', 'Backspace', 'Enter'];
  // each opening: letters first, then the symbols page, then the keyboard closes (a number-only opening is a different case)
  for (let i = 0; i < 4; i++) { showKeys(a, 'all'); await sleep(80); showKeys(a, sym); await sleep(80); showKeys(a, []); await sleep(80); }
  showKeys(a, []); await sleep(80);
  assert.equal(a.report().keyboard.issue, '');
});

// ---- blocked eval / content-security policy ---------------------------------------------------------------
test('a page that forbids eval and new Function: the layer installs, types and swipes as normal', async () => {
  const a = setup({ noEval: true }, true), b = setup({ noEval: true }, false);
  assert.throws(() => vm.runInContext('eval("1")', a.page.ctx), /EvalError|disallow|not allowed/i, 'the test page must really forbid eval');
  assert.throws(() => vm.runInContext('new Function("return 1")', a.page.ctx), /EvalError|disallow|not allowed/i);
  assert.equal(a.L.gestures.state(), 'on');
  assert.equal(a.L.session.state(), 'on');
  await same(a, b, async (r) => { for (const k of ['q', 'w']) { await tap(r, k); await sleep(30); } });
  await swipe(a, ['q', 'p'], 60); await settle();
  assert.equal(a.calls.length >= 1, true, 'the swipe still reaches the binding');
  assert.equal(a.L.failsafe !== null, true);
});

test('the layer source has no eval, no Function constructor, no string timers, no inline style or script text', () => {
  const dir = path.join(__dirname, '..', '..', 'gaming');
  const files = ['layer/gesture.js', 'layer/session.js', 'layer/failsafe.js', 'helper/swipekb_helper/layer_stub.js'];
  for (const f of files) {
    const src = fs.readFileSync(path.join(dir, f), 'utf8').split('\n').filter((l) => !/^\s*\/\//.test(l)).join('\n');
    for (const bad of [/\beval\s*\(/, /new\s+Function\b/, /\bFunction\s*\(/, /setTimeout\s*\(\s*['"`]/, /setInterval\s*\(\s*['"`]/,
      /innerHTML|outerHTML|insertAdjacentHTML/, /setAttribute\s*\(\s*['"]style['"]/, /createElement\s*\(\s*['"](?:script|style)['"]/, /document\.write/]) {
      assert.ok(!bad.test(src), f + ' uses ' + bad);
    }
  }
});

// ---- nothing left behind ----------------------------------------------------------------------------------
test('layer removed while a tap is being held back: the letter still types, once, and no listener is left', async () => {
  const a = setup({}, true), b = setup({}, false);
  for (const r of [a, b]) {
    const f = r.f(); const [x, y] = r.c('k'); f.down(x, y);
    if (r === a) r.L.remove();
    await sleep(30); f.up();
  }
  await settle();
  assert.deepEqual(a.S.typed, ['k']);
  assert.deepEqual(a.S.typed, b.S.typed);
  assert.equal(a.win.listenerCount(), 0);
});

test('layer removed in the middle of a swipe: no stray letter, nothing stuck, the next tap types', async () => {
  const a = setup({}, true);
  const f = a.f(); const [x, y] = a.c('q'); f.down(x, y);
  for (let i = 1; i <= 6; i++) { f.move(x + i * 30, y); await sleep(4); }
  a.L.remove();
  for (let i = 7; i <= 9; i++) { f.move(x + i * 30, y); await sleep(4); }
  f.up(); await settle();
  assert.deepEqual(a.S.typed, []);
  await tap(a, 'w'); await settle();
  assert.deepEqual(a.S.typed, ['w']);
});

test('a layer that is removed leaves no timer and no marker, even when it is removed before the page has finished loading', async () => {
  const a = setup({}, true);
  assert.ok(a.live.size > 0, 'the running layer has timers');
  a.L.remove();
  assert.equal(a.live.size, 0, 'timers left behind: ' + a.live.size);
  assert.equal(a.win.listenerCount(), 0);
  // At document start there is no <html> yet: the frame waits for DOMContentLoaded to write its marker.
  const page = build(); const html = page.doc.documentElement;
  page.doc.documentElement = null;
  const added = [];
  page.doc.addEventListener = (t, fn) => added.push([t, fn]);
  vm.runInContext(fs.readFileSync(process.env.SWIPEKB_STUB_LONG, 'utf8'), page.ctx);
  const L = page.win.__swipekbLayer; rigs.push({ L });
  assert.ok(added.some(([t]) => t === 'DOMContentLoaded'));
  L.remove();
  page.doc.documentElement = html;
  for (const [, fn] of added) fn();
  assert.equal(html.getAttribute('data-swipekb-stub'), null, 'a removed layer wrote its marker after all');
});

test('a hook that throws while a tap is handed back to Steam still gives Steam the tap, once', async () => {
  const a = setup({}, true), b = setup({}, false);
  a.L.session.hooks.keyTyped = () => { throw new Error('boom'); };
  await tap(a, 'k'); await tap(b, 'k'); await settle();
  assert.deepEqual(a.S.typed, ['k'], 'the tap was lost or doubled');
  assert.deepEqual(a.S.typed, b.S.typed);
  assert.equal(a.L.gestures.state(), 'failed');
  assert.equal(a.win.listenerCount(), 0);
  a.S.typed.length = 0; await tap(a, 'j'); await settle();
  assert.deepEqual(a.S.typed, ['j']);
});

test('a hook that throws as a held touch is handed to Steam still gives Steam the press, so the lift types the letter', async () => {
  const a = setup({}, true);
  a.L.session.hooks.uncertain = () => { throw new Error('boom'); };
  const f = a.f(); const [x, y] = a.c('d'); f.down(x, y);
  await sleep(300);                     // past the 180 ms hand-off: the hook runs and throws
  f.up(); await settle();
  assert.deepEqual(a.S.typed, ['d']);
  assert.equal(a.L.gestures.state(), 'failed');
  assert.equal(a.win.listenerCount(), 0);
});

test('the watcher failing on its own switches only itself off', async () => {
  const a = setup({}, true); showKeys(a, []);
  const n = a.live.size;
  Object.defineProperty(a.doc, 'activeElement', { get() { throw new Error('page broke'); } });
  await sleep(150);
  assert.equal(a.live.size, n - 1, 'the watcher kept its timer');
  assert.equal(a.L.gestures.state(), 'on');
  assert.equal(a.L.session.state(), 'on');
});

test('the report is numbers and words about the keyboard, never anything typed', async () => {
  const a = setup({}, true);
  await tap(a, 'k'); await settle();
  const raw = a.L.report();
  const rep = JSON.parse(raw);
  assert.deepEqual(Object.keys(rep).sort(), ['gestures', 'gestures_reason', 'keyboard', 'session', 'session_reason']);
  assert.deepEqual(Object.keys(rep.keyboard).sort(), ['issue', 'max_letters', 'no_keys_run', 'opens', 'short_run']);
});

test('a part that cannot report comes back as unknown, and the report is still valid', async () => {
  const a = setup({}, true);
  a.L.session = { state() { throw new Error('odd'); } };
  a.L.gestures = {};
  const rep = JSON.parse(a.L.report());
  assert.equal(rep.session, 'unknown');
  assert.equal(rep.gestures, 'unknown');
});
