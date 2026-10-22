// Gesture arbiter tests (plan M4). Run through tests/gaming/test_layer.py (or `node --test tests/gaming/test_layer.js`).
// Every behaviour is checked against the Steam-keyboard model in fake_keyboard.js, with the layer on AND off,
// because the promise is "typing is identical to stock", not "typing works".
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { build, loadLayer, sleep } = require('./fake_keyboard.js');

const LETTERS = [...'qwertyuiopasdfghjklzxcvbnm'];

function rig(opts, config, layer) {
  const page = build(opts);
  const L = layer === false ? null : loadLayer(page, config);
  return { page, L, S: page.S, c: page.centre, f: () => page.finger() };
}
async function tap(r, name, hold) {
  const f = r.f(); const [x, y] = r.c(name);
  f.down(x, y); if (hold) await sleep(hold); f.up(); return f;
}
// A swipe: path of points, total duration ms, ~8 ms per step like touchd.py.
async function swipe(r, pts, ms) {
  const f = r.f(); f.down(pts[0][0], pts[0][1]);
  const steps = Math.max(2, Math.floor(ms / 8)); const per = ms / steps;
  for (let i = 1; i <= steps; i++) {
    const t = i / steps * (pts.length - 1); const a = Math.min(pts.length - 2, Math.floor(t)); const u = t - a;
    f.move(pts[a][0] + (pts[a + 1][0] - pts[a][0]) * u, pts[a][1] + (pts[a + 1][1] - pts[a][1]) * u);
    if (per >= 1) await sleep(per);
  }
  f.up(); return f;
}
const settle = () => sleep(60);

test('every letter tapped once types exactly once, ten rounds, same as stock', async () => {
  for (const withLayer of [false, true]) {
    const r = rig({}, null, withLayer);
    const want = [];
    for (let round = 0; round < 10; round++) for (const ch of LETTERS) { await tap(r, ch); want.push(ch); }
    await settle();
    assert.deepEqual(r.S.typed, want, 'layer ' + (withLayer ? 'on' : 'off'));
    if (r.L) r.L.gestures.off();
  }
});

test('a tap reaches Steam as one down and one up of each family, and no browser mouse events follow (as stock)', async () => {
  const r = rig();
  await tap(r, 'g'); await settle();
  assert.deepEqual(r.S.log.slice().sort(), ['pointerdown', 'pointerup', 'touchend', 'touchstart']);
});

test('fifty swipes of varied speed leave no stray letters and are all recorded', async () => {
  const r = rig();
  const calls = []; r.page.win.__swipekb = (s) => calls.push(JSON.parse(s));
  const paths = [['q', 'p'], ['a', 'l'], ['z', 'm'], ['h', 'e', 'l'], ['t', 'o', 'p'], ['g', 'y']];
  for (let i = 0; i < 50; i++) {
    const p = paths[i % paths.length].map((n) => r.c(n));
    await swipe(r, p, [24, 40, 80, 160, 320][i % 5]);
  }
  await settle();
  assert.deepEqual(r.S.typed, []);
  assert.equal(calls.length, 50);
  assert.ok(calls.every((s) => s.trace.length >= 3 && s.keys.length === 26), 'every stroke has a trace and the 26 de-duplicated keys');
  assert.equal(r.L.gestures.stats().swipe, 50);
});

test('the same swipes WITHOUT the layer do leave a stray letter (the test can fail)', async () => {
  const r = rig({}, null, false);
  await swipe(r, [r.c('q'), r.c('p')], 80); await settle();
  assert.deepEqual(r.S.typed, ['q']);
});

test('movement of exactly 12 px is still a tap; 13 px is a swipe', async () => {
  const r = rig();
  let f = r.f(); let [x, y] = r.c('k'); f.down(x, y); f.move(x + 12, y); f.up(); await settle();
  assert.deepEqual(r.S.typed, ['k']);
  r.S.typed.length = 0;
  f = r.f(); f.down(x, y); f.move(x + 13, y); f.up(); await settle();
  assert.deepEqual(r.S.typed, []);
});

test('the swipe path keeps its start, end, order and times', async () => {
  const r = rig(); const got = []; r.page.win.__swipekb = (s) => got.push(JSON.parse(s));
  const [x1, y1] = r.c('a'), [x2, y2] = r.c('l');
  await swipe(r, [[x1, y1], [x2, y2]], 120);
  const tr = got[0].trace;
  assert.ok(Math.abs(tr[0][0] - x1) < 1 && Math.abs(tr[0][1] - y1) < 1);
  assert.ok(Math.abs(tr[tr.length - 1][0] - x2) < 1);
  for (let i = 1; i < tr.length; i++) assert.ok(tr[i][2] >= tr[i - 1][2], 'time never goes backwards');
  assert.equal(got[0].v, 1);
});

test('Shift then a letter, one finger after the other, gives the capital exactly as stock', async () => {
  for (const withLayer of [false, true]) {
    const r = rig({}, null, withLayer);
    await tap(r, 'Shift'); await sleep(30); await tap(r, 'h'); await tap(r, 'i'); await settle();
    assert.deepEqual(r.S.typed, ['H', 'i'], 'layer ' + withLayer);
  }
});

test('second finger while the first is on Shift passes straight through (two-thumb Shift+letter)', async () => {
  for (const withLayer of [false, true]) {
    const r = rig({}, null, withLayer);
    const a = r.f(), b = r.f();
    a.down(...r.c('Shift')); await sleep(20);
    b.down(...r.c('h')); b.up(); a.up(); await settle();
    // Shift is only a latch in Steam's model, so the exact result is whatever stock does; layer must match
    r.S.typed.push('|'); a.down(...r.c('Shift')); a.up(); await settle();
    if (!withLayer) rig.stock = r.S.typed.slice(); else assert.deepEqual(r.S.typed, rig.stock);
  }
});

test('second finger landing while the first is waiting on a letter: both letters type, like stock', async () => {
  for (const withLayer of [false, true]) {
    const r = rig({}, null, withLayer);
    const a = r.f(), b = r.f();
    a.down(...r.c('a')); await sleep(10); b.down(...r.c('s')); await sleep(10); b.up(); await sleep(10); a.up(); await settle();
    assert.deepEqual(r.S.typed.slice().sort(), ['a', 's'], 'layer ' + withLayer);
  }
});

test('non-letter keys reach Steam synchronously and untouched (no swallow, no delay)', async () => {
  for (const k of ['Shift', 'Space', 'Backspace', 'Enter']) {
    const r = rig();
    const f = r.f(); f.down(...r.c(k));
    assert.ok(r.S.log.includes('pointerdown') && r.S.log.includes('touchstart'), k + ' down arrived at once');
    assert.equal(r.L.gestures.stats().swallowed, 0);
    f.up(); await settle();
  }
});

test('Space, Enter and Backspace taps type once, same as stock', async () => {
  const seq = async (withLayer) => {
    const r = rig({}, null, withLayer);
    for (const k of ['Space', 'Enter', 'Backspace']) await tap(r, k);
    await settle(); return r.S.typed;
  };
  assert.deepEqual(await seq(true), await seq(false));
});

test('Backspace held keeps stock repeat: same count of repeats and the first backspace is not delayed', async () => {
  const run = async (withLayer) => {
    const r = rig({}, null, withLayer);
    const f = r.f(); const t0 = r.page.now(); f.down(...r.c('Backspace')); await sleep(1500); f.up(); await settle();
    return { n: r.S.typed.length, first: r.S.keydownAt[0] - t0 };
  };
  const off = await run(false), on = await run(true);
  assert.ok(on.n >= 5, 'repeating happened: ' + on.n);
  assert.ok(Math.abs(on.n - off.n) <= 1, `repeat count on ${on.n} vs off ${off.n}`);
  assert.ok(Math.abs(on.first - off.first) <= 40, `first repeat on ${on.first} vs off ${off.first}`);
});

test('a letter held past the hold limit then released types once (popup not yet open) like stock', async () => {
  for (const withLayer of [false, true]) {
    const r = rig({}, null, withLayer);
    await tap(r, 'a', 300); await settle();
    assert.deepEqual(r.S.typed, ['a'], 'layer ' + withLayer);
  }
});

test('long-press accent popup is the same with the layer on and off, and sliding onto an accent types it', async () => {
  const run = async (withLayer) => {
    const r = rig({}, null, withLayer);
    const f = r.f(); f.down(...r.c('e')); await sleep(800);
    const items = r.page.doc.querySelectorAll('[data-key-is-literal]').map((e) => e.getAttribute('data-key'));
    const pop = r.page.doc.querySelector('[data-key-is-literal]');
    const target = [pop.rect.left + 75 + 5, pop.rect.top + 20]; // third child
    f.move(target[0], target[1]); await sleep(30); f.up(); await settle();
    return { items, typed: r.S.typed };
  };
  const off = await run(false), on = await run(true);
  assert.ok(off.items.length === 8);
  assert.deepEqual(on, off);
});

test('while a popup is open every touch passes through', async () => {
  const r = rig();
  const a = r.f(); a.down(...r.c('e')); await sleep(800);
  const before = r.S.log.length; const b = r.f(); b.down(...r.c('s'));
  assert.ok(r.S.log.slice(before).includes('pointerdown'), 'second finger reached Steam');
  b.up(); a.up(); await settle();
});

test('a still touch held 3 s releases normally', async () => {
  const r = rig();
  await tap(r, 'd', 3000); await settle();
  assert.deepEqual(r.S.typed, ['d']);
  await tap(r, 'f'); await settle();
  assert.deepEqual(r.S.typed, ['d', 'f']);
});

test('a slow 3 s swipe does not get stuck and the next tap types', async () => {
  const r = rig();
  await swipe(r, [r.c('a'), r.c('l')], 3000); await settle();
  assert.deepEqual(r.S.typed, []);
  await tap(r, 'f'); await settle();
  assert.deepEqual(r.S.typed, ['f']);
});

test('swipe results go to the binding; a missing or throwing binding changes nothing for the user', async () => {
  for (const mode of ['missing', 'throws']) {
    const r = rig();
    if (mode === 'throws') r.page.win.__swipekb = () => { throw new Error('daemon dead'); };
    await swipe(r, [r.c('a'), r.c('l')], 60); await tap(r, 'x'); await settle();
    assert.deepEqual(r.S.typed, ['x'], mode);
    assert.equal(r.L.gestures.state(), 'on');
  }
});

test('an exception in a handler removes the layer and the next tap still types', async () => {
  const r = rig(); const w = r.page.win;
  assert.ok(w.listenerCount() > 0);
  r.L.gestures.injectFault();
  await tap(r, 'g'); await settle(); // the fault fires on this touch
  assert.equal(r.L.gestures.state(), 'failed');
  assert.equal(w.listenerCount(), 0, 'no listeners left on window');
  assert.deepEqual(r.S.typed, ['g'], 'the in-flight tap was replayed, not lost');
  await tap(r, 'h'); await settle();
  assert.deepEqual(r.S.typed, ['g', 'h']);
});

test('an exception while a swipe is in progress does not type a stray letter or wedge the keyboard', async () => {
  const r = rig();
  const f = r.f(); f.down(...r.c('q')); await sleep(20); f.move(...r.c('w')); r.L.gestures.injectFault(); f.move(...r.c('e')); f.up(); await settle();
  assert.equal(r.L.gestures.state(), 'failed');
  await tap(r, 'a'); await settle();
  assert.ok(r.S.typed.includes('a'));
});

test('key geometry counts each wide key once and ignores accent-popup items', async () => {
  const r = rig({ nestedWide: true });
  assert.ok(r.page.doc.querySelectorAll('[data-key]').length > 26 + 4, 'the page really has duplicate nodes');
  const g0 = r.L.gestures.keys();
  assert.equal(g0.length, 26 + 4);
  assert.equal(g0.filter((k) => k.key === 'Backspace').length, 1);
  const a = r.f(); a.down(...r.c('e')); await sleep(800);
  const g1 = r.L.gestures.keys();
  assert.ok(r.page.doc.querySelectorAll('[data-key-is-literal]').length > 0);
  assert.equal(g1.length, 26 + 4, 'popup items are not keys');
  a.up(); await settle();
});

test('with fewer than 24 letter keys on screen (symbols page) the layer does nothing', async () => {
  const r = rig({ noLetters: true });
  const f = r.f(); f.down(...r.c('3'));
  assert.ok(r.S.log.includes('pointerdown'), 'reached Steam at once');
  f.up(); await settle();
  assert.deepEqual(r.S.typed, ['3']);
});

test('password box: nothing is stopped, nothing is sent', async () => {
  const r = rig(); const calls = []; r.page.win.__swipekb = (s) => calls.push(s);
  const inp = r.page.doc.createElement('input'); inp.setAttribute('type', 'password'); r.page.doc.body.append(inp); r.page.doc.activeElement = inp;
  const f = r.f(); f.down(...r.c('a'));
  assert.ok(r.S.log.includes('pointerdown'), 'down reached Steam untouched');
  f.up(); await settle();
  assert.deepEqual(r.S.typed, ['a']);
  await swipe(r, [r.c('q'), r.c('p')], 60); await settle();
  assert.equal(calls.length, 0);
});

test('a cancelled touch types nothing', async () => {
  const r = rig();
  const f = r.f(); f.down(...r.c('a')); await sleep(20); f.cancel(); await settle();
  assert.deepEqual(r.S.typed, []);
  await tap(r, 'b'); await settle();
  assert.deepEqual(r.S.typed, ['b']);
});

test('a lost touch-end does not wedge the layer: it lets go after the stuck limit', async () => {
  const r = rig({}, { gesture: { stuck_ms: 150 } });
  const f = r.f(); f.down(...r.c('a')); f.move(...r.c('s')); // swipe in progress ... and then no more events
  await sleep(400);
  assert.equal(r.L.gestures.stats().stuck, 1);
  await tap(r, 'b'); await settle();
  assert.deepEqual(r.S.typed, ['b']);
});

test('touch events only (no pointer events): taps type once, swipes leave nothing', async () => {
  for (const fam of ['touch', 'pointer']) {
    const r = fam === 'touch' ? rig({ touchOnly: true, steamFamily: 'touch' }) : rig({ pointerOnly: true });
    await tap(r, 'a'); await swipe(r, [r.c('q'), r.c('p')], 60); await tap(r, 'b'); await settle();
    assert.deepEqual(r.S.typed, ['a', 'b'], fam);
  }
});

test('removing the layer puts every listener back and stock typing resumes', async () => {
  const r = rig();
  r.L.gestures.off();
  assert.equal(r.page.win.listenerCount(), 0);
  await tap(r, 'a'); await settle();
  assert.deepEqual(r.S.typed, ['a']);
});

test('installing twice does not stack listeners', async () => {
  const r = rig(); const n = r.page.win.listenerCount();
  const L2 = loadLayer(r.page); void L2;
  assert.equal(r.page.win.listenerCount(), n, 'second install replaced, not added');
  await tap(r, 'a'); await settle();
  assert.deepEqual(r.S.typed, ['a']);
});

// ---- the frame the helper really installs (layer_stub.js + gesture.js, assembled by the helper itself) -------
function installStub(page) {
  assert.ok(process.env.SWIPEKB_STUB_FILE, 'run through tests/gaming/test_layer.py, which assembles the stub with the helper');
  require('vm').runInContext(require('fs').readFileSync(process.env.SWIPEKB_STUB_FILE, 'utf8'), page.ctx);
  return page.win.__swipekbLayer;
}

test('the installed frame starts the gestures, keeps them after a failure, and removes them with itself', async () => {
  const page = build(); const f1 = page.finger();
  const L = installStub(page);
  assert.ok(L.gestures, 'gestures started');
  assert.equal(page.doc.documentElement.getAttribute('data-swipekb-gestures'), 'on');
  const g = f1; g.down(...page.centre('a')); g.up(); await settle();
  assert.deepEqual(page.S.typed, ['a']);
  // a failure removes the gesture listeners but the frame stays, so the helper's heartbeat does not reinstall them
  L.gestures.injectFault(); const f2 = page.finger(); f2.down(...page.centre('b')); f2.up(); await settle();
  assert.equal(L.gestures.state(), 'failed');
  assert.equal(page.win.listenerCount(), 0);
  assert.equal(L.beat(), L.version, 'heartbeat still answers with the version');
  assert.equal(page.doc.documentElement.getAttribute('data-swipekb-gestures'), 'failed');
  L.remove();
});

test('removing the frame (or the helper going silent) takes the touch listeners off', async () => {
  const page = build(); const L = installStub(page);
  assert.ok(page.win.listenerCount() > 0);
  L.remove();
  assert.equal(page.win.listenerCount(), 0);
  const page2 = build(); installStub(page2);
  await sleep(1200); // the test config uses a 600 ms watchdog and the helper is not beating
  assert.equal(page2.win.listenerCount(), 0, 'orphaned layer removed itself and its listeners');
  assert.equal(page2.win.__swipekbLayer, undefined);
});

test('with letters on screen but fewer than 24 (another script or a partial layout) the layer does nothing', async () => {
  const r = rig({ letterRows: 1 });
  const f = r.f(); f.down(...r.c('q'));
  assert.ok(r.S.log.includes('pointerdown'), 'the touch reached Steam at once, nothing was held back');
  f.up(); await settle();
  assert.deepEqual(r.S.typed, ['q']);
});

test('a second finger landing while the first waits on a letter releases the first at once', async () => {
  const r = rig(); const a = r.f(), b = r.f();
  a.down(...r.c('a')); assert.equal(r.S.downs.size, 0, 'first finger is being held back');
  b.down(...r.c('s'));
  assert.equal(r.S.downs.size, 2, 'Steam now knows about both fingers');
  b.up(); a.up(); await settle();
});

test('an exception while a tap is being held back still types that tap', async () => {
  const r = rig();
  const f = r.f(); f.down(...r.c('a')); assert.equal(r.S.downs.size, 0);
  r.L.gestures.injectFault();
  f.up(); await settle();                       // the fault fires on this release
  assert.equal(r.L.gestures.state(), 'failed');
  assert.deepEqual(r.S.typed, ['a'], 'the held-back tap was given to Steam, not lost');
  await tap(r, 'b'); await settle();
  assert.deepEqual(r.S.typed, ['a', 'b']);
});

test('a letter touched while an accent pop-up is on screen goes straight to Steam', async () => {
  const r = rig();
  const p = r.page.doc.createElement('div'); r.page.kb.append(p);       // a pop-up left on screen
  const item = r.page.doc.createElement('div'); p.append(item); item.setAttribute('data-key', 'é'); item.setAttribute(LIT, '1');
  const f = r.f(); f.down(...r.c('s'));
  assert.ok(r.S.log.includes('pointerdown'), 'reached Steam at once, nothing held back');
  f.up(); await settle();
});
const LIT = 'data-key-is-literal';
