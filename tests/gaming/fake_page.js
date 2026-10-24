// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// Stand-in for one Steam UI page: a persistent Node vm context with just enough of `window`/`document`
// to run the helper's real layer stub. Line protocol on stdin/stdout, one JSON object per line.
//   {op:'init', steam:null|'ok'|'nomethod'|'nosteamclient'}
//   {op:'reset', scripts:[...]}     new document: old timers die with it, then each script runs (UI reload)
//   {op:'binding', name}           Runtime.addBinding: window[name](payload) is collected, see 'drain'
//   {op:'drain'}                   -> {out:[[name, payload], ...]} binding calls made since the last drain
//   {op:'eval', id, expr}           -> {id, ok:true, value} | {id, ok:false, error}
// WHY a real JS engine and not a Python imitation: the stub's own code (timers, guards, removal) is what the
// tests are meant to prove, so it must actually run.
const vm = require('vm');
const readline = require('readline');
let ctx = null;
let steam = null;
let origin = 'https://steamloopback.host';
let timers = [];
let bindings = [];   // names the helper registered with Runtime.addBinding; they survive a new document like Chrome's
let outgoing = [];   // [name, payload] pairs the page called, waiting to be collected

function newContext() {
  for (const t of timers) { clearInterval(t); clearTimeout(t); }
  timers = [];
  const attrs = {};
  const sb = {
    document: {
      readyState: 'complete',
      documentElement: {
        setAttribute: (k, v) => { attrs[k] = String(v); },
        getAttribute: (k) => (k in attrs ? attrs[k] : null),
        removeAttribute: (k) => { delete attrs[k]; },
      },
      addEventListener() {}, removeEventListener() {},
    },
    setInterval: (f, ms) => { const t = setInterval(f, ms); timers.push(t); return t; },
    clearInterval: (t) => clearInterval(t),
    setTimeout: (f, ms) => { const t = setTimeout(f, ms); timers.push(t); return t; },
    clearTimeout: (t) => clearTimeout(t),
    Date, JSON, Object,
  };
  for (const name of bindings) sb[name] = (p) => { outgoing.push([name, String(p)]); };
  ctx = vm.createContext(sb);
  vm.runInContext('var window = globalThis; var top = globalThis; var location = {origin: ' + JSON.stringify(origin) + '};', ctx);
  if (steam === 'ok') vm.runInContext('var __sent = []; var SteamClient = {Input: {ControllerKeyboardSendText: function(t){ __sent.push(String(t)); }}};', ctx);
  if (steam === 'nomethod') vm.runInContext('var SteamClient = {Input: {}};', ctx);
  if (steam === 'badtype') vm.runInContext('var SteamClient = {Input: {ControllerKeyboardSendText: 5}};', ctx);
}

function out(o) { process.stdout.write(JSON.stringify(o) + '\n'); }
newContext();
readline.createInterface({ input: process.stdin }).on('line', (line) => {
  const m = JSON.parse(line);
  if (m.op === 'init') { steam = m.steam; if (m.origin) origin = m.origin; newContext(); out({ id: m.id, ok: true }); }
  else if (m.op === 'binding') { if (!bindings.includes(m.name)) bindings.push(m.name); ctx[m.name] = (p) => { outgoing.push([m.name, String(p)]); }; out({ id: m.id, ok: true }); }
  else if (m.op === 'drain') { out({ id: m.id, ok: true, out: outgoing.splice(0) }); }
  else if (m.op === 'reset') {
    newContext();
    for (const s of m.scripts || []) { try { vm.runInContext(s, ctx); } catch (e) { /* page keeps loading */ } }
    out({ id: m.id, ok: true });
  } else if (m.op === 'eval') {
    try { out({ id: m.id, ok: true, value: vm.runInContext(m.expr, ctx) }); }
    catch (e) { out({ id: m.id, ok: false, error: String(e && e.message || e) }); }
  }
});
