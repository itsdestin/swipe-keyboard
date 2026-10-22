// Gaming Mode layer STUB (milestone M3). It does nothing a user can see: it only proves the install path.
// The helper replaces __CONFIG__ with JSON and runs this in Steam's keyboard page, both right now and at the
// start of every new document in that page (so a UI reload brings it straight back).
(function (CONFIG) {
  var w = window;
  // WHY top frame only: the script also runs in any iframe the page makes; the marker belongs to the page itself.
  if (w.top !== w) return;

  var old = w.__swipekbLayer;
  if (old) {
    // WHY: a helper that restarted finds the stub already here. Same version means "leave it alone" (no double
    // install); an older one is removed first so two layers never stack.
    if (old.version === CONFIG.version) { if (old.beat) old.beat(); return; }
    try { old.remove(); } catch (e) { /* an old stub that cannot remove itself is replaced anyway */ }
  }

  var ATTR = 'data-swipekb-stub';
  var L = {
    version: CONFIG.version,
    installs: ((old && old.installs) || 0) + 1,
    installedAt: Date.now(),
    lastBeat: Date.now(),
    config: CONFIG.device,   // what the real layer will check at each keyboard open (shared contract)
    timer: null
  };

  function mark() {
    var el = document.documentElement;
    if (el) el.setAttribute(ATTR, L.version);
  }

  L.beat = function () { L.lastBeat = Date.now(); mark(); return L.version; };

  L.remove = function () {
    if (L.timer !== null) { clearInterval(L.timer); L.timer = null; }
    var el = document.documentElement;
    if (el && el.getAttribute(ATTR) === L.version) el.removeAttribute(ATTR);
    if (w.__swipekbLayer === L) delete w.__swipekbLayer;
  };

  // WHY a watchdog: if the helper crashes (kill -9) nobody can send "remove". The helper pings every second;
  // when the pings stop the layer takes itself out, so a dead helper never leaves half a layer in Steam.
  L.timer = setInterval(function () {
    if (Date.now() - L.lastBeat > CONFIG.watchdog_ms) L.remove();
  }, Math.max(100, Math.floor(CONFIG.watchdog_ms / 4)));

  w.__swipekbLayer = L;
  mark();
  // At document start there may be no <html> yet; mark again once the page has been built.
  if (!document.documentElement && document.addEventListener) document.addEventListener('DOMContentLoaded', mark);
})(__CONFIG__);
