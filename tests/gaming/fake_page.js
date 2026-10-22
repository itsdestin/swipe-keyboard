// Stand-in for one Steam UI page: a persistent Node vm context with just enough of `window`/`document`
// to run the helper's real layer stub. Line protocol on stdin/stdout, one JSON object per line.
//   {op:'init', steam:null|'ok'|'nomethod'|'nosteamclient'}
//   {op:'reset', scripts:[...]}     new document: old timers die with it, then each script runs (UI reload)
//   {op:'eval', id, expr}           -> {id, ok:true, value} | {id, ok:false, error}
// WHY a real JS engine and not a Python imitation: the stub's own code (timers, guards, removal) is what the
// tests are meant to prove, so it must actually run.
const vm = require('vm');
const readline = require('readline');
let ctx = null;
let steam = null;
let timers = [];

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
  ctx = vm.createContext(sb);
  vm.runInContext('var window = globalThis; var top = globalThis;', ctx);
  if (steam === 'ok') vm.runInContext('var SteamClient = {Input: {ControllerKeyboardSendText: function(t){}}};', ctx);
  if (steam === 'nomethod') vm.runInContext('var SteamClient = {Input: {}};', ctx);
  if (steam === 'badtype') vm.runInContext('var SteamClient = {Input: {ControllerKeyboardSendText: 5}};', ctx);
}

function out(o) { process.stdout.write(JSON.stringify(o) + '\n'); }
newContext();
readline.createInterface({ input: process.stdin }).on('line', (line) => {
  const m = JSON.parse(line);
  if (m.op === 'init') { steam = m.steam; newContext(); out({ id: m.id, ok: true }); }
  else if (m.op === 'reset') {
    newContext();
    for (const s of m.scripts || []) { try { vm.runInContext(s, ctx); } catch (e) { /* page keeps loading */ } }
    out({ id: m.id, ok: true });
  } else if (m.op === 'eval') {
    try { out({ id: m.id, ok: true, value: vm.runInContext(m.expr, ctx) }); }
    catch (e) { out({ id: m.id, ok: false, error: String(e && e.message || e) }); }
  }
});
