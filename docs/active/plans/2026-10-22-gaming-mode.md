---
status: active
---
# Gaming Mode support (Steam Deck / SteamOS)

Planning document only. No product code exists yet. Facts marked **spike** were measured in the SteamOS Gaming Mode VM on 2026-10-22; facts marked **unverified** need the Milestone 0 probes before anything depends on them.

## Plain-language summary (for the owner)

**What you will get.** On a Steam Deck in Gaming Mode, Steam's own on-screen keyboard keeps working exactly as it does today. Swipe Keyboard adds two things on top of it: you can swipe across the letters to type a word, and a row of word suggestions appears just above the keys. Everything else about Steam's keyboard (look, controller typing, emoji, languages, the key that closes it) stays Valve's.

**How it works, in everyday terms.** Steam's keyboard is really a small web page. Steam has a developer switch that lets other programs on the Deck look at and add to its screens. We use that switch. A tiny background program (the "helper") notices when Steam starts, attaches to the keyboard page and adds our swipe-and-suggest layer. A second background program (the "keyboard brain") does the word-guessing using the same engine as Desktop Mode. The helper is the only piece that is allowed to talk over the network connection, and only because it has to reach Steam on the same device. The brain has no network at all. Nothing ever goes to the internet.

**If anything goes wrong, you simply have Steam's normal keyboard.** If the helper is not running, if Steam changed something we do not understand, or if our layer notices anything odd, it switches itself off and does nothing. Tapping letters still types letters. It can never leave you without a keyboard.

**Passwords.** In Steam's own password boxes (sign-in, family PIN, Wi-Fi password) our layer does absolutely nothing: no swipe, no suggestions, nothing is recorded. When you pop the keyboard open over a game or another program, Steam does not tell anyone what kind of box it is, so we cannot know whether it is a password box. There, swipe and suggestions work, but nothing you type is ever saved or learned, and everything we hold is erased when the keyboard closes. We cannot honestly promise more than that, and the README will say so.

**What you will feel.** Tapping a letter now types it when your finger lifts, not when it lands. That is about 80 thousandths of a second later than stock. Most people will not notice; fast typists might. Keys that are not letters (Backspace, Shift, Space, Enter, numbers) stay instant. This delay is the price of telling a tap from a swipe, because Steam types the instant a finger lands and we cannot take a letter back.

**Surviving restarts and updates.** Everything we install lives in your home folder, not in the part of SteamOS that gets replaced whole on each system update. After a reboot, switching between Gaming and Desktop Mode, a Steam update or a SteamOS update, the helper starts again by itself and re-attaches within a few seconds. The one thing that can need a Steam restart is the developer switch itself, if Steam ever forgets it. We will test every one of these cases in the VM before release; some are not yet proven (marked below).

**Can Valve break it?** Yes, any time, and we cannot prevent that. Valve could close the developer switch, put a password on it, rename the parts we use, or make their keyboard into something we cannot attach to. Valve ships that switch on purpose and a large community tool (Decky Loader) has used the same door for about four years without Valve closing it, but that is not a promise. What we can do: keep the number of Steam internals we rely on very small, check them on the device every time the keyboard opens, check Valve's public Steam files every day from our own server, and fall back silently to the stock keyboard. If Valve closes the door on purpose, the fallbacks give less (suggestions only, or a second separate keyboard), and building them takes weeks, not days. If Valve only reshuffles internal names, we expect a fix within days.

**Terms of service.** Steam's subscriber agreement forbids modifying Steam software without Valve's written consent. Attaching to Steam's screens is a grey area under that sentence. It touches no game and no anti-cheat files, and it changes none of Steam's files on disk, but it is not Valve-approved. You accepted this; the README says it in plain words (draft in Section 12). This is not legal advice.

**Decisions I need from you** are collected in Section 15. The most important: how fixes reach people if the product has no internet (today updates are manual downloads).

## 1. Scope and non-goals

In scope (v1): SteamOS on Steam Deck and other SteamOS 3 devices, Gaming Mode, installed through the existing per-user Flatpak route; English, Spanish, French and German using the layouts Steam offers for those; swipe plus suggestion strip over Steam's keyboard on Steam-owned screens, over non-Steam apps, and over running games.

Fixed by the owner (not reopened): additive layer injected into Steam's keyboard page through Steam's debug port by a project-owned helper (no Decky); commit through Steam's own text call; native engine reused as a local program; stock keyboard always works underneath and on any failure; known password fields fully inert; unknown field types swipe and suggest on but learn and store nothing; no internet of any kind (no remote kill switch, no online compatibility list); helper and engine split with a local socket between; one installer; testable in the SteamOS VM; daily canary on Valve's public Steam UI files.

Not in v1: native Arch/Fedora packages (they are not SteamOS; they stay unchanged), Bazzite/ChimeraOS/CachyOS-handheld sessions (same design should work, untested), non-Latin layouts (layer stays off), physical Deck performance claims (owner pass only, Milestone 11), controller-driven selection of suggestions (open question 6).

## 2. What was proven (spike) and what was not

Proven, with scripts in the cache spike folder (`gaming-spike/`): the debug port opens with the file `~/.steam/steam/.cef-enable-remote-debugging` plus a Steam restart (port 8080, also 8081); Steam's UI has pages named `Steam Big Picture Mode` (the whole UI and the keyboard, 1280x800) and `SharedJSContext` (invisible; holds the real `SteamClient`); the keyboard keys are `[data-key]` elements with row, column, keycode and extended-character attributes and exact rectangles; a finger drag arrives as one pointer-down, many moves and one pointer-up with true coordinates; Steam types a key on pointer-down, so a swipe leaves a stray letter unless our layer swallows the events first; swallowing plus replaying the tap on release works for plain letters; closing and reopening the keyboard keeps our listeners (same page document), only a new page (Steam restart, UI reload) needs re-injection; the page's own `SteamClient` is a stripped 7-method object, so the commit must go through `SharedJSContext` (two targets, bridged by a CDP binding); one `ControllerKeyboardSendText("hello ")` call types a whole word (page-to-character 54 to 103 ms in five samples); the same flow works in the logged-in library search, over a running game (Spacewar under Proton: same page, tap replay delivered Return to the game), and over a non-Steam editor (kwrite: word commit, taps, Backspace, swipe with no stray letter); `steam -ifrunning steam://open/keyboard` opens the keyboard over a game or app (it is the call Valve's Qt/GTK input modules make).

Password signals found: `document.activeElement.type` is `password` on the sign-in password field, `text` on the account field, `search` in library search. Over a game or app opened with `steam://open/keyboard` the page has zero `<input>` elements and `activeElement` is `BODY`: no field-type signal at all.

Not proven (each is a Milestone 0 probe or later test): long-press accent popups, Shift hold, key repeat and non-letter replay; commit of control characters (`\b`, `\n`); a typed word landing in a game's own text field; the game-requested keyboard (Steamworks text-input call) and its password flag; whether user-level systemd units start in the Gaming Mode session; whether the debug port is bound to localhost only; whether the marker file survives a Steam client update; whether a SteamOS image update leaves everything working; behaviour with the real Steam+X chord and on physical touch hardware; whether a Python interpreter is present in the KDE Flatpak runtime we ship on.

## 3. Architecture

```
SteamOS (read-only root, replaced wholesale by OS updates; everything below lives in /home)

  ~/.config/systemd/user/                         ~/.local/share/flatpak/ (--user installs)
    swipekb-engine.service  ------------------>     org.itsdestin.SwipeKeyboard           (existing app; NO network)
    swipekb-helper.service  ------------------>       swipekb-gamingd   (engine daemon, new command)
         (WantedBy=default.target,                  org.itsdestin.SwipeKeyboard.GamingHelper (new app;
          Restart=always)                                              network, localhost use only)
                                                      swipekb-helper    (Python, stdlib only)

   Steam client (not ours, untouched files)
   ┌──────────────────────────────────────────────────────────────────────────────┐
   │ CEF debug port 127.0.0.1:8080  (on because ~/.steam/steam/.cef-enable-remote-debugging exists)
   │                                                                               │
   │  page "Steam Big Picture Mode"            page "SharedJSContext"              │
   │  = Steam UI + Steam's own keyboard        = hidden; real SteamClient          │
   │    + OUR LAYER (injected, in memory)        SteamClient.Input.                │
   │      gesture arbiter, trail, strip,           ControllerKeyboardSendText      │
   │      policy gate, contract check                                              │
   └───────▲───────────────────┬───────────────────────────▲──────────────────────┘
           │ Runtime.evaluate  │ CDP binding               │ Runtime.evaluate
           │ (results back)    │ __swipekb (page -> helper)│ (commit)
   ┌───────┴───────────────────▼───────────────────────────┴──────────────────────┐
   │ swipekb-helper                                                                │
   │   find targets (/json/list) -> inject -> watch for new targets -> re-inject   │
   │   validate + rate-limit every message; commit only text the engine issued     │
   │   status file ~/.local/state/swipekb-gaming/status.json (no network)          │
   └───────────────────────────────┬──────────────────────────────────────────────┘
                                   │ unix socket, mode 0600, $XDG_RUNTIME_DIR/swipekb/engine.sock
                                   │ (shared directory granted to both apps only)
   ┌───────────────────────────────▼──────────────────────────────────────────────┐
   │ swipekb-gamingd (engine daemon; no network permission; lazy model load)       │
   │   recognizer + n-gram + word model (existing engine/), policy owner           │
   └───────────────────────────────────────────────────────────────────────────────┘

 Failure rule at every arrow: do nothing, remove our listeners, stock keyboard keeps working.
 Taps and non-letter keys never need the helper or the engine; only swipes and the strip do.
```

Design rules that keep the Steam-facing surface small (details in Section 10):
- The layer depends only on: DOM `[data-key]` attributes and rectangles, standard pointer/touch events, `document.activeElement.type`, and one Steam call (`SteamClient.Input.ControllerKeyboardSendText` in `SharedJSContext`). No React internals, no wrapping of Steam's functions, no `SteamUIStore`, no webpack module lookup, no file edits, no binary patching.
- Everything the layer reads is read-only. The only things it changes are DOM nodes it creates itself (trail canvas, strip) and capture-phase event listeners on `window`.
- Every dependency is feature-detected at injection and again at each keyboard open, against `gaming-contract.json` (Section 11), the same file the daily canary checks.

## 4. Components and file locations

New files (all inside this repository; names are proposals):

| Component | Location | Notes |
|---|---|---|
| Engine daemon | `gaming/daemon/main.cpp`, `gaming/daemon/protocol.hpp`, `gaming/daemon/CMakeLists.txt` | Links the existing `engine/` library (`recognizer.hpp`, `prediction.hpp`, `ngram.hpp`, `nlm.hpp`). Binary `swipekb-gamingd`, installed in the main Flatpak. No Qt GUI, no network code. Length-prefixed JSON over `AF_UNIX`. |
| Helper | `gaming/helper/swipekb_helper/` (`__main__.py`, `ws.py` minimal loopback WebSocket client, `cdp.py`, `inject.py`, `commit.py`, `status.py`, `config.py`) | Python standard library only, to avoid a dependency chain; asks for no third-party wheels. Own Flatpak app. Ported from `gaming-spike/bridge.py` and `cdp.py`. |
| Layer | `gaming/layer/` (`contract.js`, `gesture.js`, `policy.js`, `strip.js`, `trail.js`, `main.js`) | Plain JavaScript, no framework, built into one file by `scripts/build-layer.py` (concatenation, no bundler, no network). Evolved from `gaming-spike/overlay2.js`. Ships as data inside the helper Flatpak and is never written into Steam. |
| Contract | `gaming/contract/gaming-contract.json` | Machine-readable list of anchors (Section 11). Read by the layer at runtime and by the canary in CI. One source of truth. |
| Install and uninstall script | `gaming/setup/install.sh`, `gaming/setup/uninstall.sh`, `gaming/setup/swipekb-gaming-setup.py` | Runs as the ordinary user on the host (no sudo), shows the consent text, writes the unit files and marker file, records what it did. |
| Systemd units | `gaming/setup/units/swipekb-engine.service`, `swipekb-helper.service` | Copied to `~/.config/systemd/user/`. |
| Helper Flatpak manifest | `packaging/flatpak/org.itsdestin.SwipeKeyboard.GamingHelper.json` (+ `.desktop`, metainfo) | `finish-args`: `--share=network` and `--filesystem=xdg-run/swipekb` only. See Section 9 on why `--share=network` cannot be narrowed to localhost. |
| Main Flatpak changes | `packaging/flatpak/org.itsdestin.SwipeKeyboard.json` | Adds `swipekb-gamingd`, adds `--filesystem=xdg-run/swipekb`, still no network. Desktop Mode behaviour unchanged. |
| Canary | `tools/steam-canary/` (`fetch.py`, `check_contract.py`, `ledger.py`), `.github/workflows/steam-canary.yml` | Section 11. |
| VM test tools | `tools/gaming-vm/` (`touchd.py`, `cdp.py`, `touch.sh`, `run-acceptance.py`) | Adapted from the spike; need root only inside the guest (uinput). |
| Tests | `tests/gaming/` (layer unit tests run under Node in CI with synthetic pointer events, helper tests with a fake CDP server, protocol tests, contract tests with recorded Steam UI fixtures), `tests/packaging/test_flatpak_gaming.py` | Fixtures contain only public Valve UI fragments needed for anchors, never screenshots. |

Existing files that change: `packaging/release-targets.json` (adds the helper bundle and installer to the Flatpak target's file list), `scripts/release.py` and `.github/workflows/packages.yml` (build and checksum the extra bundle), `tests/packaging/test_flatpak.py` and `test_release*.py` (exact file lists), `README.md`, `docs/install-details.md` (replace "Gaming Mode is not supported"), `docs/releasing.md` (the "does not certify Gaming Mode" sentence), `packaging/flatpak/setup-guide.py` (mention the opt-in step). Native recipes (`packaging/arch/PKGBUILD`, `packaging/fedora/swipe-keyboard.spec`) do not change; a packaging test asserts they ship no gaming files.

## 5. Install, uninstall, upgrade on SteamOS

Only the Flatpak route applies to SteamOS (the README already forbids the Arch/Fedora packages on a Deck). Nothing touches `/usr`, `/etc` or the read-only root, and no `sudo` is used.

**Install (Desktop Mode, Konsole).** The existing Desktop Mode steps stay exactly as they are for people who only want Desktop Mode. Gaming Mode is an extra opt-in step:
1. Install both Flatpak bundles with `flatpak install --user` (the main app and `GamingHelper`).
2. Run `install.sh` (a release asset, SHA-256 listed in `SHA256SUMS`). It: shows the plain-language consent text and the exact list of changes; asks `[y/N]`; creates `~/.local/share/Steam/.cef-enable-remote-debugging` (and the `~/.steam/steam` path if it is a separate directory), recording in `~/.local/share/swipe-keyboard-gaming/install-record.json` whether the file already existed (Decky users already have it; we must not delete it on uninstall); writes the two unit files to `~/.config/systemd/user/`; runs `systemctl --user daemon-reload` and `enable --now` for both; prints "Switch to Gaming Mode. If Steam was already running, restart it once (the install can do this for you if no game is running)". It never restarts Steam by itself while a game is running.
3. In Gaming Mode, touch a text field. Steam's keyboard opens; swipe.

**Why a host script and not the sandbox.** A Flatpak app cannot write units into `~/.config/systemd/user` or Steam's folder without broad filesystem permission, and starting units through the system bus would be a sandbox escape. A short, auditable shell script run by the user is the least-privilege route. Consent text and changes are printed before anything is done. The script is idempotent (running twice is harmless) and has `--dry-run`.

**Units.** `swipekb-engine.service` runs `flatpak run --user --command=swipekb-gamingd org.itsdestin.SwipeKeyboard`; `swipekb-helper.service` runs `flatpak run --user --command=swipekb-helper org.itsdestin.SwipeKeyboard.GamingHelper`. Both `WantedBy=default.target`, `Restart=always`, `RestartSec=3`. The units refer to app ids and commands, not to version paths, so a Flatpak upgrade never makes them stale. All logic that changes between versions lives in the Flatpaks, not in the units.

**Idle cost.** The engine daemon stays running but loads models only on the first swipe request and unloads them after 10 idle minutes; the helper is a small polling loop. Budget to confirm in Milestone 2: engine under 30 MB when idle, helper under 40 MB, under 1% CPU when Steam is running and idle. (The desktop keyboard once stayed resident at about 440 MB on SteamOS; this design must not repeat that.)

**Upgrade.** Install the newer bundles over the old ones (same command as today). `install.sh --upgrade` rewrites unit files only if their embedded version stamp is older, restarts both units, and re-checks the marker file. Settings and the install record are kept. Downgrade is refused unless `--force`.

**Uninstall.** `uninstall.sh`: stop and disable both units; remove the unit files; remove the marker file only if the install record says we created it; remove `~/.local/share/swipe-keyboard-gaming/` and `~/.local/state/swipekb-gaming/`; print the two `flatpak uninstall --user` commands (or run them with `--remove-apps`). Offers to leave the debug switch on if the user wants. Because our layer lives only in Steam's memory, nothing remains in Steam after its next restart; the script says so. Removing the Flatpak without running uninstall leaves units that fail to start; they are harmless (`Restart=always` throttled by `StartLimit`) and Section 8's removal guide covers cleanup.

**Native packages.** Unchanged in v1. A test asserts the Arch and Fedora file lists contain nothing under `gaming/`.

## 6. Persistence: surviving every kind of restart

Principle: nothing we need lives in the replaced OS image. The cases below are each a Milestone 8 test. "Verify" means not yet proven.

| Event | What happens | Survives? | Mechanism and test |
|---|---|---|---|
| Deck reboot | Autologin starts the user session; the user manager starts our units | Verify | Units are `WantedBy=default.target` in `~/.config/systemd/user` (in `/home`). Test: reboot the guest, wait for Gaming Mode, assert both services active and the layer injected without any manual step. If user units do not start in the Gaming Mode session, fallback order: a `gamescope-session` drop-in is not allowed (outside `/home`), so use a Steam-launched route: a hidden non-Steam shortcut is rejected (it writes the library); the realistic fallback is enabling lingering with `loginctl enable-linger` (needs one polkit prompt) or an XDG autostart `.desktop` that only runs in Desktop Mode (not enough). Milestone 0 decides. |
| Gaming Mode to Desktop Mode and back | The Gaming session ends; Desktop session starts; the user manager may stop and restart | Verify | Units restart with the new login. In Desktop Mode the helper idles (desktop Steam, if running, has the same debug port; layer only attaches to a page that has the keyboard keys). Test: `steamosctl switch-to-desktop-mode` and back, ten times; assert re-injection within the bound after every return. |
| Steam client restart (crash, user "Restart Steam", Steam update restart) | Port closes, all targets die, new targets with new ids appear | Yes by design | The helper loop: connect to `127.0.0.1:8080/json/version`; when it fails, retry every 1 s capped at 3 s; when it answers, subscribe to target events (`Target.setDiscoverTargets`; fall back to polling `/json/list` every 1 s if the events are absent, unverified); on any new or changed `Steam Big Picture Mode` or `SharedJSContext` target, inject within one cycle. **Bound: layer installed within 2 s of the Big Picture page existing (polling) and within 5 s of the port opening.** Also `Page.addScriptToEvaluateOnNewDocument` so a plain page reload re-injects with no round trip. Test: kill Steam by pid 20 times, record timestamps. |
| Layer already present | Helper reconnects after its own restart | Yes | Layer has a version and an `alreadyInstalled` guard; helper compares and replaces if its own version is newer. |
| `.cef-enable-remote-debugging` file and the Developer setting across Steam updates | Steam reads the file only at startup; a Steam started before the file existed ignores it (spike) | Verify | The helper recreates the file every time it starts and every 60 s while it is missing (cheap check). If the port is closed while Steam is running, status becomes `needs-steam-restart`; we do not restart Steam automatically while a game runs. Test: switch the VM to the Steam beta channel (write the channel name into `package/beta`, as the guest does) and let Steam update; assert file still present, port open after restart. Second path: Valve's own "CEF Remote Debugging" Developer setting (stored in Steam's config in `/home`, survives updates by design); the installer does not toggle it, but the settings text tells users it is an equivalent switch. |
| SteamOS image update (A/B partitions, root replaced) | `/usr` and `/etc` defaults replaced; `/home` kept; Flatpak user installs live in `~/.local/share/flatpak` | Verify | Real test: in the stable guest overlay run `steamos-update` to the next image if the VM can reach it; if not, simulate by booting the beta guest image with the stable guest's `/home` disk attached. Assert units, flatpak apps, marker file and layer all work, and that no unit references a path outside `/home`, `/app` or the Flatpak runtime. |
| Flatpak update (runtime or our bundles) | Apps replaced in place | Yes | Units name app ids. Test: `flatpak update --user` of the runtime, then install a newer test bundle; assert services restart cleanly and the engine socket permission still works (overrides are per-app and persist). |
| Debug port in use by Decky | Decky owns nothing exclusive on the port; both can be clients | Verify | CDP allows several clients. Test: run a stand-in second client; assert both work. |

**Status visibility.** The helper writes `~/.local/state/swipekb-gaming/status.json` (state, reason, Steam build, layer version, last inject time). It is read by the Desktop Mode settings page (a new "Gaming Mode" line: Active, Waiting for Steam, Paused because Steam changed, Needs a Steam restart, Off). Gaming Mode itself shows nothing intrusive: failure is silent by design.

## 7. Interaction design (inside Steam's keyboard page)

**Gesture arbiter** (evolved from `overlay2.js`). Every pointer/touch event is captured on `window` before Steam sees it. Rules, in order:
1. Password or inert state (Section 8): do nothing at all, call nothing, stop nothing.
2. A second finger lands while one is already down: pass it through immediately (replay down at once, forward everything after). This is how Shift-then-letter and two-thumb typing keep working.
3. First finger lands on a non-letter key (Backspace, Shift, Space, Enter, symbol/number keys, emoji, layout/language, arrows, the Move/Close keys): pass through immediately with no delay. Backspace and Shift keep exact stock feel, including key repeat and Shift-hold.
4. First finger lands on a letter key: swallow it and wait.
   - Moves more than 12 px before lifting: swipe. Record the stroke, swallow everything, nothing reaches Steam.
   - Lifts without moving: tap. Replay a synthetic pointer+touch down and up on the key; Steam types it.
   - Held still for at least 180 ms (tuning knob): treat as a long-press. Replay the pointer-down now (not the up) and pass everything after through, so Steam's own accent popup, its timers and key repeat run unmodified. Cost: a swipe that begins after a 180 ms rest types one stray letter. Mitigation: swipes normally start moving at once; the threshold is tuned on real hardware (Milestone 11) and exposed as a hidden setting; if this proves common we move to a lower threshold plus an "undo stray letter" backspace.
5. Any exception, timeout (touch held over 1.5 s with no events from Steam's side), or contract failure: remove all listeners, replay the in-flight tap if any, mark the layer failed for this keyboard session.

**Latency trade-off, plainly.** Taps on letter keys type about 80 ms after stock (the finger-down-to-finger-up time measured in the spike). It cannot be removed while Steam types on pointer-down; it can be shortened by replaying early on a still finger (a 60 ms "early tap"), but that collides with long-press and with slow swipe starts, so v1 ships release-time replay and Milestone 11 measures whether an early-replay option is worth it. Alternatives rejected: wrapping Steam's key handler to undo a stray letter (needs Steam internals, which we refuse to depend on); hiding Steam's keys under our own (a second keyboard, loses Steam's look and controller use).

**Accent popups, Shift, caps and symbols.** Not handled by us at all after the pass-through rules; Milestone 0 observes exactly how Steam shows the popup (which DOM, whether it uses `data-extended-chars`) so a test can prove it appears the same with the layer on and off. Swiping starting on an accent-bearing key works because recognition uses key centres only; accented letters in a candidate word are produced by the engine's own normaliser and committed as text.

**Swipe flow.** Stroke ends -> layer sends `{id, trace[], keys[], context[], lang}` through the binding -> helper validates -> daemon returns up to 3 candidates -> helper evaluates a call in the page -> strip fills. The top candidate is committed on release as in Desktop Mode (word plus trailing space). Choosing another candidate: tap it; the layer asks the helper to commit the replacement, first erasing the word it committed (Section 7.4). Target budget: swipe release to candidate visible under 120 ms excluding engine compute (spike: 0.1 ms helper forwarding; loopback IPC unmeasured).

**7.4 Commit and erase.** Commit text with a single `ControllerKeyboardSendText(word + " ")` in `SharedJSContext`. Replacing or erasing a committed word needs backspaces; Milestone 0 tests whether `"\b"` through that call works; if not, the layer replays N synthetic Backspace key taps (it knows N). The "first Backspace removes the whole swiped word" behaviour of Desktop Mode is a v1.1 item behind this test; v1 may use plain one-letter Backspace.

**Suggestion strip placement.** A fixed-position element created by us (class prefix `swipekb-`), three slots, strongest in the centre, sitting directly above the top row of Steam's keys. Its position is computed from the bounding rectangle of the top-most `[data-key]` row minus the strip height, recomputed on resize, on layout change, and at each keyboard open. It draws over the app or game (the keyboard is already an overlay at the bottom of the screen) and never moves Steam's own elements. Steam's own IME row (used for Chinese/Japanese/Korean candidates) is not used and not touched. Touches on the strip are ours (they do not start on a `[data-key]`, so Steam ignores them). The trail is a pointer-events-none canvas over the key area, cleared 300 ms after the stroke ends. If the strip would cover Steam's own top IME row (when a CJK layout is active) the layer is off anyway.

**Typing context.** We do not wrap Steam's key handler. We learn what was typed because we replay every letter tap ourselves (we know the key) and see every pass-through key press by its `data-key`/`data-keycode`; plus our own commits. This gives a shadow buffer of text typed since the keyboard opened. It is reset on keyboard close, field change (`activeElement` changes), Enter, arrows, the Move key, and whenever an unknown key or a held-key popup makes the buffer uncertain. Limits, to be stated in the docs: text already in the field is invisible to us in unknown mode (we start with no context); where the field is a Steam DOM input we may read its value and caret for context, never persist it.

## 8. Privacy and password rules, mapped to real signals

| Situation | Signal found in the spike | Behaviour |
|---|---|---|
| Steam password field (sign-in, family PIN, Wi-Fi password, server password) | `document.activeElement` is `<input type="password">`, evaluated on every pointer-down in the page, no CDP round trip | **Fully inert.** The layer does not capture, delay, draw, call the binding, record, or send anything. Steam handles the touch untouched. Keyboard appears and behaves as stock. |
| Steam fields of known plain kinds | `activeElement.type` is `text`, `search`, `email`, `url`, `tel`, `number` | Swipe and suggestions on. Context may be read from the field value for suggestions. Learning: off in v1 (open question 3). |
| Field type missing or unrecognised, or no input at all (keyboard opened over an app or game with Steam+X or `steam://open/keyboard`) | `activeElement` is `BODY`, zero `<input>` elements, no Enter-label change (spike) | **Unknown mode** (owner decision): swipe and suggestions on; nothing learned; nothing written to disk by the layer, helper or daemon; the shadow buffer lives in page memory and is wiped on close; daemon keeps nothing between requests and logs no text. A small "Private" chip on the strip switches the rest of that keyboard session to inert (open question 4). The strip hides while the typed run looks like a secret (long, no spaces, mixed case plus digits plus symbols). |
| Game-requested keyboard (Steamworks text-input call), password mode | Reported to render a real `<input>` with `type="password"` when the game asks for password mode (`m_eInputMode`); **unverified live** | If it renders as a password input it is covered by row 1. Milestone 6 tests it with a Steamworks sample; until proven, treat as unknown mode. |
| Daemon and helper | n/a | No text is ever written to disk in Gaming Mode. Logs contain counts and timings only, never words, keys or traces. Status file holds states, not text. |

What we can and cannot promise (goes into the README): password boxes **inside Steam** are inert; password boxes inside **other programs or games** reached through Steam's keyboard cannot be detected by anyone; there, nothing is saved or learned and suggestions show only words from the current session.

Test for inert (Milestone 5): with the password field focused, a counting spy proves zero calls to `stopImmediatePropagation`, zero binding calls, no strip node in the DOM, and an empty file-system diff of every directory the apps can write (before/after listing with checksums). The same filesystem diff runs in unknown mode after typing and swiping in kwrite.

## 9. Security of the debug port

What the switch is: a standard Chrome DevTools port on Steam's UI, listening for connections. Anyone who can reach it can run script inside Steam's UI. That script can use Steam's UI interfaces (launch games, change library entries as the spike's shortcut add/remove showed, read UI state). It is a real privilege, and it is the same exposure every Decky Loader user already accepts.

Facts to verify in Milestone 0 (not assumed): that the port listens on `127.0.0.1` only and not on the Wi-Fi address (`ss -ltn`, plus a connection attempt from the host side of the VM); that browsers cannot reach it from web pages (Chromium checks the Host header and requires an allowed Origin for WebSocket connections; non-browser clients such as our helper send no Origin).

Who on the Deck can reach it: any program running as the same user (including any Flatpak app that has network access), and any other user account. Web pages cannot, if the checks above hold.

Mitigations we control:
- We add no new listening port anywhere. Page-to-helper traffic uses the CDP binding, not TCP. Helper-to-engine uses a Unix socket with mode 0600 in a directory only the two apps share (`--filesystem=xdg-run/swipekb`), plus a per-boot random token file in that directory checked by the daemon (guards against other Flatpak apps that were given the same directory).
- The helper connects only to `127.0.0.1` and only to WebSocket URLs returned by `/json/list` that are loopback addresses on the same port.
- The injected script is a static file packaged in the Flatpak; nothing is fetched at runtime, nothing is `eval`'d from the network. The helper sends only that script and fixed result calls.
- Anything arriving from the page through the binding is untrusted (the page is Steam's UI and could host content we did not write): strict size and rate limits; schema validation; **the commit path accepts only a candidate id previously issued by the daemon for that exact request**, so the binding can never be used as a "type any text" primitive.
- The helper's Flatpak has `--share=network` because that is the only way a sandbox reaches the host's loopback; Flatpak cannot limit it to localhost. So the "helper has network permission" statement is true and the helper is deliberately tiny, holds no user data and no models, and contains no code that opens non-loopback connections. A test greps the helper source for socket creation and forbids any host other than `127.0.0.1`. The engine and models stay in the app that has no network.
- One-click off: `install.sh --disable` stops the helper and removes the marker file (if we created it). The Desktop settings page shows the debug-port state.
- Stated in the README: while the switch is on, programs on the device can control Steam's screens.

Residual risk we cannot remove: the switch stays on in Desktop Mode too, because it is one file read by Steam at start. Optional hardening (open question 5): create the file when the Gaming session starts and remove it at logout; rejected as default because it races Steam's own start and makes the layer unreliable.

## 10. Could Valve intentionally break this? An honest assessment

We cannot prevent it. We can make it safe, notice it the same day, and shorten the fix.

**Ways Valve could block it, from most to least likely and what each would cost:**

| Valve action | Effect | Evidence and likelihood | What we do |
|---|---|---|---|
| Rename or restructure internal names, page titles or the SteamClient method | Layer's anchors fail | Frequent for general UI code: Decky needed Steam-update fix releases about every 4 months for its component library (2.11.1 Feb 2024 to 3.3.0 Oct 2026) and had about six user-reported breakage episodes in the first ten months of 2026, nearly always on the beta channel first (proposal C, from GitHub). That breakage was in Decky's reuse of internal UI components, which this design avoids. A spike-era anchor set was identical across three builds spanning 8 days; long history is unmeasured. | Minimise anchors (Section 3); runtime contract check; daily canary; adapter updates are small data changes. Expect keyboard-specific breakage every several months, not weeks (an estimate, not a measurement). |
| Change `data-key` attributes or move the keys out of ordinary DOM | Layer cannot find keys; no gesture capture | A third-party plugin written in Oct 2025 against the same keyboard still matches today's build; the keys have been DOM attributes through the builds compared. | Contract check refuses to enable below 24 recognised letter keys with sane geometry; stock keyboard stays. |
| Rename or remove `SteamClient.Input.ControllerKeyboardSendText` | No commit path | It is Valve's own keyboard's only text exit and is a property name, so minification cannot rename it; a rename would be deliberate. | Fall back: replay synthetic key taps for each letter (slow, no accents); then the IBus route. |
| Content-security policy or script-integrity checks on the Steam UI | Injected script refuses to run | Not observed. CDP `Runtime.evaluate` bypasses page CSP by design, so only an engine-level block would stop it. | Detection by behavioural canary; fallback routes. |
| Detect injected scripts or listeners | Valve could disable the feature or flag it | Not observed; not found in the bundle in our reading. No information on policy. | Same. Our listeners are the ordinary kind a web page adds; nothing is hidden. |
| Lock or remove the debug port, or require a token/authentication on it | Entire approach dead until Valve allows it | This is the decisive one. Valve ships the switch deliberately (a Developer-menu "CEF Remote Debugging" setting exists in Steam's own text) and the community has used the port for about four years without it being closed. But it is documented nowhere as a promise, it is a developer feature, and an upstream browser-engine upgrade could change how the flag behaves (Chromium has tightened remote-debugging defaults before; unverified whether it affects Steam's build). | Fallbacks below. Be clear with users. |
| Ship swipe or a new keyboard themselves | Feature obsoleted | No sign of swipe, autocorrect or suggestion code in Steam's keyboard. | We stop gladly. |
| Contract or legal action under the subscriber agreement | Product withdrawn | Grey area; no known enforcement against Decky; not legal advice. | We would comply and remove the feature. |
| Anti-cheat | Account risk | We touch no game process; the injection is into Steam's own UI. We know of no report of enforcement tied to this; not verified. Stated as a risk, not a guarantee. | Documented. |

**Design choices that reduce exposure to breakage:** no React-internal access; no wrapping or patching of Steam's functions; read-only selectors; every dependency feature-detected; fail to nothing; daily check of the public bundle plus a behavioural canary for what static checks cannot see; layer and helper are small, so a fix is a small change and a normal release.

**What the static canary cannot see:** the debug port's behaviour lives in Steam's native client, not the UI bundle. Two guards: (a) the canary also downloads the native client packages named in the same manifests and checks for the marker-file name and the debug-port option strings (exact packages identified in Milestone 1); (b) the weekly VM behavioural canary actually opens the port and injects. A silent native-only change is therefore caught by (b) within a week at worst, and users still fail safe to stock.

**Fallback pathways, kept ready (design notes now, build only if needed):**

| Pathway | Delivers | Does not deliver | Status |
|---|---|---|---|
| F0 Stock Steam keyboard | Typing as today | Swipe, suggestions | Always the base. |
| F1 IBus candidate strip: our engine as an IBus engine feeding Steam's own IME candidate row | Word completion and next-word suggestions in Valve's own UI; clean with respect to Steam's terms | Swipe (IBus sees keys, not traces); password awareness (Steam hard-codes a free-form content type). | Unverified that `ibus-daemon` runs in Gaming Mode, that a user-supplied engine loads, and the Steam English-engine override race. Milestone 0 only checks daemon presence. Estimated 2-3 weeks. |
| F2 gamescope route: our own small keyboard window shown over the game, typing through gamescope's private input-method protocol | Swipe and suggestions without Steam's debug port | Steam's look and controller typing; field-type signals; relies on window properties meant for Steam and a protocol labelled "regular Wayland clients must not use it"; fragile against compositor changes. | Fallback if the port is closed. Estimated 4-6 weeks. |
| F3 Decky plugin wrapper around the same layer | Users who already run Decky get its re-attachment | Nothing if the port itself is blocked | Optional convenience only; not planned. |

Everything above is bounded by the same statement: if Valve closes the port on purpose, the full experience cannot be restored by adapting; F1 or F2 give less.

## 11. Daily canary

**Purpose:** learn the same day Valve publishes a Steam UI build that no longer matches our contract, without a Deck, a Steam account, or the internet on user devices.

**What it downloads** (public, no login; verified in the proposals): the client manifests `steam_client_ubuntu12`, `steam_client_publicbeta_ubuntu12`, `steam_client_steamdeck_stable_ubuntu12`, `steam_client_steamdeck_publicbeta_ubuntu12` from `https://client-update.steamstatic.com/`; each names a `steamui_websrc_all` zip, `steamui_websrc_all.zip.<sha1>`. Each run: read the manifest version; if new for that channel, download the zip, check that its SHA-1 equals the name, extract into a fresh empty directory (treated as untrusted data: no execution, path-traversal checks, backslash names handled), read `sp.js` and the localisation file, then delete the directory. Also the native packages for Section 10's check once identified. Typically 35 MB per new build per channel.

**What it checks** (`gaming-contract.json` drives both the canary and the on-device check; names below are the anchors the proposals verified present in several builds, to be re-verified in Milestone 1):
1. `ControllerKeyboardSendText` exists and is still called with one text argument; `RegisterForUserKeyboardMessages`, `RegisterForGameKeyboardMessages`, `RegisterForUserDismissKeyboardMessages` exist.
2. Keyboard key rendering still emits `data-key`, `data-key-row`, `data-key-col`, `data-keycode`, and `data-extended-chars`.
3. Login/PIN screens still use `type="password"`; the game-requested keyboard route still switches to password on `m_eInputMode`.
4. The page titles our helper matches (`Steam Big Picture Mode`, `SharedJSContext`) still appear in the localisation/route names that produce them.
5. The Developer-setting string for CEF remote debugging still exists.
6. Absence-of-surprise checks that would reopen design questions: still no swipe or autocorrect code in the keyboard; Steam's IME context still created with a free-form content type (relevant to F1).
7. Drift report: the keyboard-related region of `sp.js`, with Valve's per-build number stamp removed, is hashed and compared with the last verified build; a non-empty diff does not fail but raises "review" and triggers the behavioural canary.

**Where state lives.** A dedicated `canary-ledger` branch holds one small JSON file (build number, channel, date, result, contract hash, region hash). GitHub Actions cache is not durable enough. The ledger is for people only: **no device ever reads it** (no internet at runtime).

**Alerts.** The scheduled workflow (`steam-canary.yml`, cron once a day plus manual dispatch) fails red on any anchor missing or changed; failure triggers GitHub's own notification email to the repository owner and opens or updates one tracking issue labelled `gaming-canary` with the build, channel and anchor names. A green daily run is the heartbeat; if the workflow itself stops running for three days a second job in the repository's normal CI opens an issue (guards against a silently dead canary). The issue text states: "Do not assume users are broken: devices fail safe to stock. Fix and ship a normal update."

**Behavioural canary (weekly and on any drift/review result).** Runs in the SteamOS Gaming Mode VM rig on the owner's machine (a self-hosted job, since hosted runners may lack KVM, unverified): a clean overlay that is never signed in, Steam pinned to the stable or beta channel; runs the Milestone 3 to 5 acceptance scripts against the sign-in screen (account field for swipe, password field for inertness), then writes to the same ledger. It is the only check that proves the debug port itself still works.

**Ship path after a red.** A person updates the layer or helper, runs the VM acceptance tests, and publishes a normal point release through the existing release procedure (`docs/releasing.md`: tag, required set, checksums; assets are never replaced). Because devices have no internet, a fix reaches users only when they install it (open question 1).

## 12. README and install-text changes (draft wording, needs owner approval)

README gets a new "Steam Deck: Gaming Mode" subsection with the steps in Section 5 and these paragraphs:

> **How this works, and a warning.** Gaming Mode support works by attaching to Steam's own on-screen keyboard through a developer switch that Valve provides in Steam. It adds swipe typing and suggestions on top and leaves Steam's keyboard as it is. Steam's Subscriber Agreement says you may not modify or create derivative works of software accessed through Steam without Valve's written consent. Whether this counts is unclear. We do not change any Steam file, we do not touch games or anti-cheat, and a popular community tool (Decky Loader) uses the same switch, but Valve has not approved this and could stop it or object at any time. Turning the switch on lets other programs on your Deck control Steam's screens. You can turn this off at any time. We are not lawyers; use it only if you are comfortable with that.
>
> **Passwords.** Inside Steam's own password boxes the keyboard layer does nothing. Over games and other programs, Steam does not say whether a box is a password box, so nothing is learned or saved there and suggestions are kept only until you close the keyboard.
>
> **Letters now type when you lift your finger** (about 80 ms later than before); Backspace, Shift, Space and Enter are unchanged.
>
> **If Steam changes something we depend on, Gaming Mode support switches itself off and Steam's keyboard keeps working.** Updates are manual downloads.

`docs/install-details.md`: replace "Gaming Mode is not supported" with a section covering consent, status meanings, removal, the persistence behaviour actually tested, and the unknown-field rule. `docs/releasing.md`: the supported-set sentence and the required Flatpak files list. The Release must not claim Gaming Mode on physical hardware until Milestone 11 is done.

## 13. Multilingual layouts

- Key geometry comes from the DOM for whichever layout Steam currently shows, so QWERTY, AZERTY and QWERTZ work without per-layout tables; the engine's `KeyRect` input already takes arbitrary letter positions.
- The engine language (English US/UK, Spanish, French, German) is chosen by the user's existing language setting (the daemon reads the same per-user options as Desktop Mode via the main Flatpak's data dir, which the helper app does not see; the layer asks the daemon, and the daemon answers "language not enabled" -> layer off). Open question 7 covers whether Gaming Mode should auto-pick the language from Steam's layout.
- Spanish `ñ`, French and German accented letters: accented keys live as extended characters on base keys (`data-extended-chars`); recognition uses base-key centres as the engine already does (`popupAliases`); accented words come from the engine's candidates and are committed as text. Milestone 7 tests `ñ`, `é`, `ü`, `ß` words end to end.
- Non-Latin or composition layouts (Chinese, Japanese, Korean, Cyrillic, Arabic): layer stays off. Rule: enable only if at least 24 distinct letter keys map into the active engine alphabet.
- Switching language in Steam's keyboard re-reads the keys and re-checks the rule; key rectangles are recomputed at each stroke start, not cached.

## 14. Milestones, each with a VM acceptance test

Rig: `start-gaming.sh` boots the SteamOS guest straight into real Gaming Mode (virtio-gpu with `drm_native_context=on`); screenshots by `vncshot.py`; touch only, via the persistent uinput `touchd.py` (socket commands `tap`, `drag`, `down/move/up`); `cdp.py` and the bridge talk CDP to port 8080 inside the guest. Screenshots stay out of the repository (they can show account details); tests assert on DOM, files and logs, not pictures. Recommended: use a separate throwaway Steam account for any test that needs sign-in (open question 8); the sign-in screen needs no account.

**M0. Unknowns probe (no product code; results appended to this plan).** Acceptance: a written table of verified answers: user units start in Gaming Mode after reboot and mode switches; port bound to 127.0.0.1 only; marker survives a Steam client update (switch channel, let it update) and Steam restart; `Target.*` events available on the browser websocket; what Steam does on long-press (DOM nodes, `data-extended-chars` use), Shift hold, key repeat, Backspace; whether `ControllerKeyboardSendText("\b")` and `"\n"` work (kwrite text and a newline test); Python 3 present in the KDE Flatpak runtime (else helper is C++/Qt); `ibus-daemon` presence; password flag for a Steamworks text-input sample; XDG autostart not used in Gaming Mode. Any "no" changes the plan before building on it.

**M1. Static canary.** Acceptance: workflow runs on demand and on schedule in a fork or the repository; fetches all four channels; reads contract from `gaming-contract.json`; passes on today's stable and beta; a negative control (fixture with one anchor removed, one with a renamed method) turns red and opens/updates the tracking issue; the zip with backslash names and a malicious path entry is handled safely; ledger branch updates; a "canary dead" check fires if no run for three days (simulated by date).

**M2. Engine daemon.** Acceptance: for 50 recorded swipes from the bench corpus, daemon candidates equal the plugin's candidates for the same input (existing `tools/swipekb-replay` as the oracle); runs in the main Flatpak with no network permission (`unshare -n` run passes; a connect attempt fails); idle memory and CPU within the Section 5 budget; models load lazily and unload after idle; socket mode 0600 with token; malformed and oversize requests rejected without crash; no text appears in logs (grep).

**M3. Helper core, no gestures.** Acceptance in the guest: layer stub (writes a marker into the page) is installed within 2 s of the Big Picture page existing, on 20 of 20 kills of Steam by pid and on 10 of 10 UI reloads; helper killed and restarted mid-session re-attaches without double-installing; with the marker file deleted the helper recreates it and reports `needs-steam-restart` if the port is closed; helper stopped entirely: tapping letters on the sign-in screen types normally; status file correct in each state; helper source contains no non-loopback address (test).

**M4. Gesture arbiter.** Acceptance with `touchd.py` on the sign-in account field: every letter key tapped once types exactly once (26 of 26, ten rounds); 50 scripted swipes of varied speed leave zero stray letters; two-finger Shift+letter types the capital; Backspace tap and hold keep stock latency and repeat (compare timestamps with the layer off); long-press on an accent-bearing key shows Steam's popup identical to layer-off (DOM comparison); a 3 s touch with the daemon dead still releases; deliberately thrown exception in a handler removes the layer and the next tap still types. Measure and record the letter-tap latency delta against the stock baseline (target at most 100 ms added; the spike saw about 80).

**M5. Commit, strip, trail, policy.** Acceptance: swiping `hello` on the account field enters `hello ` through `ControllerKeyboardSendText` (field value read via CDP); strip shows three candidates above the top key row at 1280x800 and at the Deck's other scale settings; tapping a candidate replaces the word; password field: spy counters prove inert (Section 8), filesystem diff empty; unknown mode: type and swipe in kwrite over the non-Steam shortcut route, diff of every writable app directory empty, buffer gone after closing the keyboard, daemon holds no state (second request without context returns context-free results); "Private" chip works; commit of a candidate not issued by the daemon is rejected.

**M6. Over apps and games.** Acceptance: the kwrite shortcut (added and removed through Steam's own shortcut API, as in the spike) and Spacewar under Proton: open the keyboard with `steam://open/keyboard` (and the real Steam+X chord if the rig can emulate it); swipe produces text in kwrite and in a game text field (a Steamworks text-input sample or a game with a name field, the spike's open item); no stray letters reach the game from swipes; stock taps still reach the game; game-requested password mode (Steamworks sample) is inert; closing and reopening the keyboard keeps working.

**M7. Multilingual.** Acceptance: for Steam layouts English US, Spanish, French (AZERTY), German (QWERTZ): swipe 20 words each reaching the engine's expected top-3; `ñ é ü ß` words commit correctly; switching layout mid-session updates geometry; a CJK layout and a Cyrillic layout leave the layer off with stock typing unaffected.

**M8. Install, uninstall, upgrade, persistence.** Acceptance: `install.sh` on a fresh guest with consent flow, `--dry-run`, idempotence, and a refusal on `N`; reboot, ten Gaming/Desktop switches, Steam restart 20 times, Steam beta-channel update, `flatpak update --user`, newer bundle install, and a SteamOS image update (or the simulated root swap) each end with the layer active within the bound and the status file correct; `uninstall.sh` removes everything it created, leaves a pre-existing marker file alone, and the guest then types on stock; native-package file lists contain no gaming files; install record accurate.

**M9. Failure injection and fallback.** Acceptance: on copies of the Steam UI with `ControllerKeyboardSendText` renamed, `data-key` removed, key count dropped, page title changed, and the debug port closed, the layer ends in Off with stock typing intact and the status shows a reason; three failures on the same Steam build suppress retries; the helper never loops against a changed Steam (retry counter capped); a CSP/blocked-eval simulation behaves the same; the F1 IBus probe is recorded as a design note with what the probe found.

**M10. Docs, packaging, release wiring, live canary.** Acceptance: packaging and release tests updated and green for the two-bundle Flatpak target; `scripts/release.py check-versions` passes; README, install-details and releasing docs changed as in Section 12; weekly behavioural canary job runs unattended once and writes the ledger; secrets scan of the whole branch clean; owner reads and approves the ToS paragraph.

**M11. Owner Deck pass (physical).** Not a VM test. Owner and a person checks on a real Deck: letter-tap feel against stock, long-press and Shift habits, strip reach with thumbs, controller use of Steam's keyboard unchanged, battery and heat with Steam idle for an hour, Gaming/Desktop switching in daily use. Tune 180 ms hold and 12 px swipe thresholds from recorded data. Only after this does the release claim Gaming Mode on hardware.

## 15. Open questions (owner decisions or unverified facts)

**Owner decisions, 2026-10-22:** all recommendations accepted for questions 1 (hosted Flatpak update repository before Gaming Mode ships), 2 (one SteamOS download), 3 (no learning in Gaming Mode v1), 4 ("Private" chip), 7 (Desktop Mode language settings), 8 (throwaway Steam account for automated tests) and 15 (Section 12 wording approved as drafted). The remaining questions are verified facts for M0 or later milestones.

1. **How do fixes reach users?** Updates today are manual downloads (`flatpak update` does not fetch a new keyboard release). A hosted Flatpak repository would let normal software updates deliver a same-week fix; it needs only static hosting and is an update channel, not an app network feature. Recommend yes before Gaming Mode ships, otherwise canary speed does not reach users. Needs owner decision.
2. **Installer shape.** Recommended: a single SteamOS download containing both Flatpaks, `install.sh` and checksums; the Desktop-only route stays as today. Alternative: keep two separate downloads. Needs owner decision.
3. **Learning in Gaming Mode.** Recommend no learning anywhere in v1 (simplest and most private). The owner rule allows learning in known plain Steam fields; that can be v1.1.
4. **"Private" chip** on the strip in unknown mode: recommend yes (one tap to inert). Cost: a visible extra element.
5. **Create the marker only for the Gaming session?** Recommended no (races Steam's start, unreliable). The debug port stays available in Desktop Mode too.
6. **Controller selection of suggestions.** Out of v1; suggestions are touch-only. Controller-only users get swipe only through the trackpad if Steam supplies pointer events (unverified).
7. **Language choice:** use the user's enabled languages from Desktop Mode settings (recommended) or follow Steam's layout automatically.
8. **Throwaway Steam account** for rig tests that need sign-in, so tests and the weekly canary never touch the owner's account. Strongly recommended.
9. **Python in the KDE runtime** for the helper (M0). If absent: write the helper in C++/Qt (QtWebSockets exists in the KDE runtime), roughly doubling the helper work.
10. **User units in the Gaming Mode session** and the fallback if they do not start (M0). Highest schedule risk for the persistence requirement.
11. **Whether the SteamOS image update test can be real** in the VM (needs the update channel to offer a newer image); otherwise the simulated root swap is the evidence and the doc says so.
12. **Real Steam+X chord** cannot be pressed without a controller; confirm the rig can synthesise the chord or accept `steam://open/keyboard` as the stand-in.
13. **Early tap replay (60 ms)** to shrink the 80 ms delay: decide with hardware data in M11.
14. **Native client check** for debug-port strings: exact packages and strings to be identified in M1.
15. **Legal wording** of the README paragraph (Section 12): owner approval; no lawyer reviewed.
16. **Anti-cheat and account risk** statement: we know of no report; a stronger claim needs evidence we do not have.
17. **Accent and Shift behaviour of Steam's keyboard** is only known after M0; the pass-through rules may need adjustment.

## 16. Acceptance of the whole plan

Complete when M0 to M10 are green in the VM, M11 is done by the owner on a real Deck, the canary has run unattended for a week with at least one deliberate-failure test, the README states the terms warning and password limits in plain words, and the stock Steam keyboard has been shown to work after every injected failure.

## M2 status (engine daemon) - 2026-10-22

Built on branch `feature/168-gm-engine-daemon`: `gaming/daemon/` (`protocol.hpp` holds the wire protocol, version 1, documented at the top of the file; `protocol.cpp`, `server.*`, `main.cpp`), tests in `tests/gaming/` (`test_protocol.cpp`, `test_daemon.py`), CTest names `gaming_protocol` and `gaming_daemon`. `tools/swipekb-replay` gained `--candidates-json` (exact guesses and scores) so it can serve as the equivalence oracle.

Measured on the host (not the Deck): first 50 swipes of the bench corpus give identical candidates and scores from the daemon and `swipekb-replay`; idle memory 11.6 MB fresh, about 200 MB with models loaded, 16.2 MB after unload (budget 30 MB); idle CPU under 1%; daemon works under `unshare -rn`.

Decisions and gaps for later milestones:
- Protocol: a "mode" per connection (`unknown` default, `plain`). Neither stores or learns text. `unknown` accepts at most 12 context words and drops secret-looking text (swipe context dropped; next-word request answered empty). Candidate ids are random, valid only for the latest request on the same connection; `commit` returns the word text only for an issued id.
- English only for now: the daemon loads the English model set. Spanish/French/German need the language-profile resolution the plugin does (M13 area). The daemon returns the engine's raw top-k; the plugin's regional re-ranking and "other spelling" insertion are not applied yet. Capitalisation is left to the layer.
- Packaging for M10: the Flatpak build now passes `-DSWIPEKB_INSTALL_GAMINGD=ON` so `/app/bin/swipekb-gamingd` exists. NOT done: the `--filesystem=xdg-run/swipekb` finish-arg (without it the socket lives in the app's private runtime dir and the helper cannot reach it), the systemd unit, adding the binary to the ELF check list in `build-swipe.sh`, and a packaging test that native Arch/Fedora packages ship no daemon (the option is off by default, which keeps them clean).
