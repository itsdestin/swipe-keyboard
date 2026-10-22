// Stand-in for Steam's on-screen keyboard page, for the gesture-layer tests (plan M4).
// WHY this exists: the layer is only worth testing against something that behaves like Steam's keyboard, so this
// file has two halves. (1) A tiny DOM with REAL capture/bubble event propagation (the layer's whole job is to
// stop events in the capture phase before Steam sees them). (2) A model of Steam's keyboard built from the M0
// measurements (plan "M0 results"): types on pointer-UP, ~555 ms long-press popup with literal children,
// three-step Shift latch (one-shot, Caps Lock, off), only Backspace repeats (~460 ms then every ~200 ms). The model is used both with the
// layer on and with it off, so "identical to stock" is a comparison, not an opinion.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

// ---- minimal DOM -------------------------------------------------------------------------------------------
function parseSel(sel) {
  // "tag[attr=val][attr2]" comma-separated; enough for the selectors the layer uses.
  return sel.split(',').map((s) => {
    const m = /^\s*([a-zA-Z]*)((?:\[[^\]]+\])*)\s*$/.exec(s);
    if (!m) throw new Error('fake DOM: unsupported selector ' + sel);
    const attrs = [];
    const re = /\[([^\]=]+)(?:=([^\]]*))?\]/g;
    let a;
    while ((a = re.exec(m[2]))) attrs.push([a[1], a[2] === undefined ? null : a[2].replace(/^["']|["']$/g, '')]);
    return { tag: m[1].toLowerCase(), attrs };
  });
}
function matches(el, parsed) {
  return parsed.some((p) => (!p.tag || el.tagName.toLowerCase() === p.tag) && p.attrs.every(([k, v]) => {
    if (!el.attrs.has(k)) return false;
    return v === null || el.attrs.get(k) === v;
  }));
}

class FakeEventTarget {
  constructor() { this.listeners = []; }
  addEventListener(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    this.listeners.push({ type, fn, capture });
  }
  removeEventListener(type, fn, opts) {
    const capture = opts === true || !!(opts && opts.capture);
    this.listeners = this.listeners.filter((l) => !(l.type === type && l.fn === fn && l.capture === capture));
  }
  listenerCount() { return this.listeners.length; }
}

class FakeEvent {
  constructor(type, init) {
    init = init || {};
    this.type = type; this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable;
    Object.assign(this, init);
    this.defaultPrevented = false; this._stop = false; this._stopNow = false; this.target = null;
  }
  stopPropagation() { this._stop = true; }
  stopImmediatePropagation() { this._stop = true; this._stopNow = true; this.stopCalls = (this.stopCalls || 0) + 1; }
  preventDefault() { this.defaultPrevented = true; }
}

class FakeElement extends FakeEventTarget {
  constructor(tag, doc) {
    super();
    this.tagName = tag.toUpperCase(); this.attrs = new Map(); this.children = []; this.parentElement = null;
    this.ownerDocument = doc; this.rect = { left: 0, top: 0, width: 0, height: 0 }; this.text = ''; this.type = '';
    this.style = {};                       // the strip and the trail are styled through this
    if (tag.toLowerCase() === 'canvas') this.ops = [];
  }
  // a canvas whose drawing calls are only recorded, so tests can see what was drawn
  getContext() {
    if (!this.ops) return null;
    const ops = this.ops; const ctx = {};
    for (const m of ['clearRect', 'beginPath', 'moveTo', 'lineTo', 'stroke']) ctx[m] = (...a) => ops.push([m, ...a]);
    return ctx;
  }
  set textContent(v) { this.text = String(v); }
  get textContent() { return this.text; }
  setAttribute(k, v) { this.attrs.set(k, String(v)); if (k === 'type') this.type = String(v); }
  getAttribute(k) { return this.attrs.has(k) ? this.attrs.get(k) : null; }
  hasAttribute(k) { return this.attrs.has(k); }
  removeAttribute(k) { this.attrs.delete(k); }
  append(c) { c.parentElement = this; this.children.push(c); this.ownerDocument.all.push(c); return c; }
  remove() {
    if (!this.parentElement) return;
    this.parentElement.children = this.parentElement.children.filter((c) => c !== this);
    const dead = new Set(); (function walk(e) { dead.add(e); e.children.forEach(walk); })(this);
    this.ownerDocument.all = this.ownerDocument.all.filter((e) => !dead.has(e));
    this.parentElement = null;
  }
  get isConnected() { return this.ownerDocument.all.includes(this); }
  getBoundingClientRect() {
    const r = this.rect; return { left: r.left, top: r.top, width: r.width, height: r.height, right: r.left + r.width, bottom: r.top + r.height, x: r.left, y: r.top };
  }
  closest(sel) {
    const p = parseSel(sel);
    for (let e = this; e; e = e.parentElement) if (matches(e, p)) return e;
    return null;
  }
  querySelectorAll(sel) {
    const p = parseSel(sel);
    const out = []; (function walk(e) { for (const c of e.children) { if (matches(c, p)) out.push(c); walk(c); } })(this);
    return out;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  dispatchEvent(ev) { return dispatch(this, ev, this.ownerDocument.win, this.ownerDocument); }
}

class FakeDocument extends FakeEventTarget {
  constructor() {
    super();
    this.all = [];
    this.documentElement = new FakeElement('html', this); this.documentElement.parentElement = null;
    this.body = new FakeElement('body', this); this.documentElement.children.push(this.body); this.body.parentElement = this.documentElement;
    this.activeElement = this.body; this.readyState = 'complete';
  }
  createElement(t) { return new FakeElement(t, this); }
  querySelectorAll(sel) { return this.documentElement.querySelectorAll(sel); }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  elementFromPoint(x, y) {
    let hit = null;
    for (const e of this.all) {
      const r = e.rect;
      if (r.width && x >= r.left && x < r.left + r.width && y >= r.top && y < r.top + r.height) hit = e; // later = deeper/on top
    }
    return hit;
  }
}

class FakeWindow extends FakeEventTarget {}

function CALL(l, self, ev) { try { l.fn.call(self, ev); } catch (e) { (CALL.errors = CALL.errors || []).push(e); } }
function dispatch(target, ev, win, doc) {
  ev.target = target;
  const chain = []; for (let e = target; e; e = e.parentElement) chain.push(e);
  chain.push(doc, win);
  // capture phase: window -> ... -> parent of target
  for (let i = chain.length - 1; i > 0 && !ev._stop; i--) {
    for (const l of chain[i].listeners.slice()) if (l.type === ev.type && l.capture) { CALL(l, chain[i], ev); if (ev._stopNow) break; }
  }
  if (!ev._stop) { // target phase: capture then non-capture
    for (const l of target.listeners.slice()) if (l.type === ev.type) { CALL(l, target, ev); if (ev._stopNow) break; }
  }
  if (ev.bubbles) for (let i = 1; i < chain.length && !ev._stop; i++) {
    for (const l of chain[i].listeners.slice()) if (l.type === ev.type && !l.capture) { CALL(l, chain[i], ev); if (ev._stopNow) break; }
  }
  return !ev.defaultPrevented;
}

// ---- the keyboard + Steam model ----------------------------------------------------------------------------
const ROWS = ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'];
const KW = 90, KH = 80, X0 = 100, Y0 = 400;

function build(opts) {
  opts = opts || {};
  const doc = new FakeDocument();
  const win = new FakeWindow();
  doc.win = win; win.document = doc; win.top = win; win.window = win; win.innerWidth = 1280; win.innerHeight = 800;
  const root = doc.body;
  const kb = new FakeElement('div', doc); root.append(kb);
  // M6/M7: Steam draws its keys inside a role=grid element whose class list holds "Layout_<its own layout name>" (found in the VM:
  // "Layout_qwerty"); layoutName null leaves the class off, an extra token stands in for Steam's hashed style classes.
  kb.setAttribute('role', 'grid');
  const setLayout = (name) => kb.setAttribute('class', '_45rH7 ' + (name === null ? '' : 'Layout_' + name + ' ') + 'DefaultTheme DialogLayout_NoMinWidth Panel');
  setLayout(opts.layoutName === undefined ? 'qwerty' : opts.layoutName);
  kb.rect = { left: 0, top: Y0, width: 1280, height: 400 };
  const keyEls = {};
  function addKey(name, x, y, w, extra) {
    // Steam's nesting (VM, M6): grid > row > gridcell > key; the layout name sits on the grid, not on the key's parent
    const rowEl = new FakeElement('div', doc); rowEl.setAttribute('role', 'row'); kb.append(rowEl);
    const cell = new FakeElement('div', doc); cell.setAttribute('role', 'gridcell'); rowEl.append(cell);
    const outer = new FakeElement('div', doc); cell.append(outer);
    outer.setAttribute('data-key', name); outer.setAttribute('data-keycode', name); outer.setAttribute('data-key-row', y); outer.setAttribute('data-key-col', x);
    outer.rect = { left: x, top: y, width: w, height: KH - 6 };
    if (extra) for (const [k, v] of Object.entries(extra)) outer.setAttribute(k, v);
    if (opts.nestedWide && w > KW) { // M0: wide keys are two nested [data-key] nodes
      const inner = new FakeElement('div', doc); outer.append(inner);
      inner.setAttribute('data-key', name); inner.setAttribute('data-keycode', name); inner.setAttribute('data-key-row', y); inner.setAttribute('data-key-col', x);
      inner.rect = { left: x + 2, top: y + 2, width: w - 4, height: KH - 10 };
      keyEls[name] = inner; outer.label = inner; return inner;
    }
    keyEls[name] = outer; return outer;
  }
  if (!opts.noLetters) {
    (opts.rows || ROWS).slice(0, opts.letterRows || 3).forEach((row, r) => [...row].forEach((ch, c) => {
      const extra = ch === 'e' ? { 'data-extended-chars': '1' } : null;
      addKey(ch, X0 + c * KW + r * 40, Y0 + r * KH, KW, extra);
    }));
  } else {
    ['1', '2', '3', '4', '5'].forEach((ch, c) => addKey(ch, X0 + c * KW, Y0, KW)); // a symbols page: too few letters
  }
  addKey('Shift', X0, Y0 + 3 * KH, 2 * KW);
  addKey(' ', X0 + 2 * KW, Y0 + 3 * KH, 4 * KW);
  addKey('Backspace', X0 + 6 * KW, Y0 + 3 * KH, 2 * KW);
  addKey('Enter', X0 + 8 * KW, Y0 + 3 * KH, 2 * KW);
  addKey('CapsLock', X0 + 10 * KW, Y0 + 3 * KH, 2 * KW);   // (Steam's real key names: ' ' for Space, 'CapsLock'; M5 VM)

  // ---- Steam model
  const S = { typed: [], log: [], latch: 0, downKey: null, popup: null, popupTimer: null, repTimer: null, repeats: 0, t0: Date.now(), keydownAt: [], upAt: [], downs: new Map() };
  const now = () => Date.now() - S.t0;
  function labelCase() { for (const [n, el] of Object.entries(keyEls)) if (n.length === 1) el.text = S.latch ? n.toUpperCase() : n; }
  labelCase();
  function type(key) { // Steam's own typing: uses the latch, clears it
    const ch = key.length === 1 && S.latch ? key.toUpperCase() : key;
    if (key.length === 1 && S.latch === 1) { S.latch = 0; labelCase(); }   // a one-shot clears with the key it capitalised; Caps Lock stays
    S.typed.push(ch === ' ' ? ' ' : ch === 'Backspace' ? '\b' : ch === 'Enter' ? '\r' : ch);
    S.keydownAt.push(now());
  }
  const family = opts.steamFamily || 'pointer';
  const typeDelay = opts.typeDelay === undefined ? 15 : opts.typeDelay; // M0: keydown 11-48 ms after the release
  function keyAt(el) { const k = el && el.closest('[data-key]'); return k; }
  function closePopup() { if (S.popup) { S.popup.remove(); S.popup = null; } }
  // WHY per finger: Steam tracks each finger on its own, so two thumbs can type at once
  const idOf = (e) => (e.changedTouches && e.changedTouches[0] ? e.changedTouches[0].identifier : e.pointerId);
  function onDown(e) {
    S.log.push(e.type);
    const k = keyAt(e.target); if (!k) return;
    const name = k.getAttribute('data-key');
    S.downs.set(idOf(e), k); S.downKey = k; S.downAt = now();
    if (k.hasAttribute('data-key-is-literal')) return;
    if (k.hasAttribute('data-extended-chars')) {
      clearTimeout(S.popupTimer);
      S.popupTimer = setTimeout(() => { // sibling div above the key, one child per accent
        const p = new FakeElement('div', doc); kb.append(p); S.popup = p; p.rect = { left: k.rect.left - 20, top: k.rect.top - 90, width: 8 * 50, height: 80 };
        [...'eéèêëēėę'].forEach((ch, i) => {
          const c = new FakeElement('div', doc); p.append(c);
          c.setAttribute('data-key', ch); c.setAttribute('data-key-is-literal', '1'); c.rect = { left: p.rect.left + i * 50, top: p.rect.top, width: 50, height: 80 };
        });
      }, 555);
    }
    if (name === 'Backspace') {
      S.repeatedThisPress = false;
      S.repTimer = setTimeout(function rep() {
        S.repeatedThisPress = true; S.repeats++; type('Backspace'); S.repTimer = setTimeout(rep, 200);
      }, 460);
    }
  }
  function onUp(e) {
    S.log.push(e.type);
    clearTimeout(S.popupTimer); clearTimeout(S.repTimer);
    S.upAt.push(now());
    const k = S.downs.get(idOf(e)); S.downs.delete(idOf(e)); S.downKey = null; if (!k) return;
    const name = k.getAttribute('data-key');
    const hit = doc.elementFromPoint(e.clientX, e.clientY);
    const lit = S.popup && hit && hit.closest('[data-key-is-literal]');
    const popupWasOpen = !!S.popup;
    closePopup();
    if (lit) { setTimeout(() => type(lit.getAttribute('data-key')), typeDelay); return; }
    if (popupWasOpen) { setTimeout(() => type(name), typeDelay); return; }
    if (name === 'CapsLock') { S.latch = S.latch === 2 ? 0 : 2; labelCase(); return; }
    if (name === 'Shift') { S.latch = (S.latch + 1) % 3; labelCase(); return; }   // M0: tap = one-shot, tap again = Caps Lock, again = off
    if (name === 'Backspace' && S.repeatedThisPress) return;
    setTimeout(() => type(name), typeDelay);
  }
  function onMove(e) { S.log.push(e.type); }
  const types = family === 'pointer' ? ['pointerdown', 'pointerup', 'pointermove'] : ['touchstart', 'touchend', 'touchmove'];
  const pt = (e) => { const t = (e.changedTouches && e.changedTouches[0]) || e; return { clientX: t.clientX, clientY: t.clientY }; };
  kb.addEventListener(types[0], onDown);
  kb.addEventListener(types[1], (e) => onUp(Object.assign(Object.create(e), pt(e), { type: e.type })));
  kb.addEventListener(types[2], onMove);
  // WHY: on the real keyboard (VM, M4) Steam calls preventDefault on the touch-end, which stops the browser from
  // adding its old-style mouse events (mousedown, mouseup, click) after a tap. A click handler that types stands in for
  // anything in Steam that would type again if those mouse events appeared; stock never lets that happen.
  kb.addEventListener('touchend', (e) => e.preventDefault());
  kb.addEventListener('click', (e) => { S.log.push('click'); const k = keyAt(e.target); if (k && opts.clickTypes !== false) type(k.getAttribute('data-key')); });
  // every other event family just gets logged so tests can see what reached Steam
  const other = family === 'pointer' ? ['touchstart', 'touchend', 'touchmove'] : ['pointerdown', 'pointerup', 'pointermove'];
  other.forEach((t) => kb.addEventListener(t, (e) => S.log.push(e.type)));

  // ---- constructors the layer expects in a browser
  class Touch { constructor(i) { Object.assign(this, i); } }
  class TouchEvent extends FakeEvent { }
  class PointerEvent extends FakeEvent { }
  class MouseEvent extends FakeEvent { }
  Object.assign(win, { Touch, TouchEvent, PointerEvent, MouseEvent, Event: FakeEvent });
  win.performance = { now: () => Date.now() - S.t0 };
  win.setTimeout = setTimeout; win.clearTimeout = clearTimeout; win.setInterval = setInterval; win.clearInterval = clearInterval;
  win.Date = Date; win.JSON = JSON; win.Math = Math; win.Object = Object; win.Array = Array; win.Set = Set; win.Map = Map; win.WeakSet = WeakSet; win.Error = Error;
  win.console = console;

  // ---- finger driver: what a touchscreen delivers (pointer event first, then touch event; touch events keep
  //      the element where the finger landed, pointer events are re-targeted by hit test)
  let nextId = 1;
  function finger() {
    const id = nextId++; const f = { id, x: 0, y: 0, target: null, isDown: false };
    const mkT = (type) => {
      const t = new Touch({ identifier: id, target: f.target, clientX: f.x, clientY: f.y });
      const list = type === 'touchend' || type === 'touchcancel' ? [] : [t];
      return new TouchEvent(type, { bubbles: true, cancelable: true, touches: list, targetTouches: list, changedTouches: [t] });
    };
    const mkP = (type) => new PointerEvent(type, { bubbles: true, cancelable: true, pointerId: id, pointerType: 'touch', isPrimary: true, clientX: f.x, clientY: f.y, button: 0, buttons: type === 'pointerup' ? 0 : 1 });
    const emit = (el, ev) => dispatch(el, ev, win, doc);
    f.down = (x, y) => {
      f.x = f.x0 = x; f.y = f.y0 = y; f.moved = false; f.isDown = true; f.target = doc.elementFromPoint(x, y) || root;
      if (!opts.touchOnly) emit(f.target, mkP('pointerdown'));
      if (!opts.pointerOnly) emit(f.target, mkT('touchstart'));
    };
    f.move = (x, y) => {
      if (Math.hypot(x - f.x0, y - f.y0) > 10) f.moved = true;
      f.x = x; f.y = y; const pt2 = doc.elementFromPoint(x, y) || root;
      if (!opts.touchOnly) emit(pt2, mkP('pointermove'));
      if (!opts.pointerOnly) emit(f.target, mkT('touchmove'));
    };
    f.up = () => {
      f.isDown = false; const pt2 = doc.elementFromPoint(f.x, f.y) || root;
      if (!opts.touchOnly) emit(pt2, mkP('pointerup'));
      if (!opts.pointerOnly) {
        const allowed = emit(f.target, mkT('touchend'));
        // the browser's compatibility mouse events follow a tap whose touch-end was not cancelled
        if (allowed && !f.moved) for (const t of ['mousedown', 'mouseup', 'click']) emit(f.target, new MouseEvent(t, { bubbles: true, cancelable: true, clientX: f.x, clientY: f.y }));
      }
    };
    f.cancel = () => { f.isDown = false; if (!opts.touchOnly) emit(f.target, mkP('pointercancel')); if (!opts.pointerOnly) emit(f.target, mkT('touchcancel')); };
    return f;
  }
  const centre = (name) => { const r = keyEls[name].rect; return [r.left + r.width / 2, r.top + r.height / 2]; };
  // opts.noEval (M9): a page whose content-security policy forbids eval and new Function, the way a locked-down Steam would be.
  const ctx = vm.createContext(win, opts.noEval ? { codeGeneration: { strings: false, wasm: false } } : undefined);
  return { win, doc, kb, keyEls, S, finger, centre, setLayout, ctx, now };
}

const GESTURE_JS = process.env.SWIPEKB_LAYER_FILE || path.join(__dirname, '..', '..', 'gaming', 'layer', 'gesture.js');
function loadLayer(page, config) {
  // The layer file defines one function; the harness plays the stub's part and calls it.
  const src = fs.readFileSync(GESTURE_JS, 'utf8');
  const L = { version: 'test', config: (config && config.device) || { keyboard_dom: { key_selector: '[data-key]', min_letter_keys: 24 }, password_signal: { input_selector: 'input[type=password]' } } };
  vm.runInContext('window = this; var __g = (function(){' + src + '\n; return swipekbGestures; })();', page.ctx);
  const g = page.ctx.__g;
  L.gestures = g(page.win, L, (config && config.gesture) || {});
  return L;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
module.exports = { build, loadLayer, sleep, dispatch, FakeEvent };
