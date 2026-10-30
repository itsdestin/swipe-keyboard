# Contributing

Swipe Keyboard adds swipe typing and word suggestions to KDE Plasma's on-screen keyboard. Help is welcome with recognition, languages, packaging, accessibility, testing and documentation. Report bugs and ideas in [GitHub Issues](https://github.com/itsdestin/swipe-keyboard/issues).

## Where things are

| Folder | What it is |
|---|---|
| `engine/` | The recognition library (swipes, completion, suggestions, learning). Start with [engine/README.md](engine/README.md) |
| `plugin/` | Keyboard add-on for Plasma 6.7/6.8 (Qt Virtual Keyboard) |
| `frontend-plasma69/` | Keyboard add-on for Plasma 6.9 |
| `integration/` | Launcher and the `swipe-keyboard` command (turning it on and off per user) |
| `gaming/` | Steam Deck Gaming Mode preview |
| `packaging/` | Arch, Fedora, Flatpak and SteamOS packages; the pinned download list `assets.lock.json` |
| `docs/models/`, `docs/methods.md` | The trained models, and how every accuracy number was measured |

Keep recognition, Qt integration, per-user activation and packaging separate. Do not add desktop layouts, tablet-mode policy or theme dependencies to the installer.

## Build and test

```sh
git clone https://github.com/itsdestin/swipe-keyboard.git
cd swipe-keyboard
python3 -m unittest discover -s tests/integration -v
```

All code is in this repository (no submodules). Data files (word lists, n-gram files, trained models) are downloaded by checksum at build time (`scripts/fetch-assets.sh`). Release downloads also include a complete source archive.

Native build requirements and commands live in the package recipes and the build workflow (`packaging/`, `.github/workflows/packages.yml`), not in a separate list that could go stale. Build in a clean distro environment. Do not use `-march=native` for binaries you redistribute. After a native build:

```sh
ctest --test-dir build --output-on-failure
```

To judge a recognition change, run the held-out swipe scoreboard (`bench/score.sh --label "description" --check`) and say which dictionary, model and ranking you used. Do not present top-three accuracy as first-guess accuracy, and do not compare results from different test sets as if they were equal. Changes that cost memory or speed must justify it.

## Safety and privacy

- Tests use temporary HOME/XDG folders. Never enable, disable, restart or replace someone's running keyboard from an automated test. A container or plugin-load test is not a touchscreen test.
- Recording is opt-in. Never attach `traces/`, learned words, typed context, credentials or unredacted logs to an issue or release. Tests that need text should use made-up phrases.

## Reporting a problem

Give the smallest steps that show it, what you expected, the package version, distribution, Plasma Keyboard and Qt versions, Wayland or X11, locale and the app. Add the hardware model if it depends on touch input.

## New platforms and languages

Required packages: Arch/CachyOS x86_64, Fedora 44 x86_64, Fedora 44 ARM64 (Plasma Mobile), Steam Deck Desktop Mode (Flatpak) and the SteamOS download for Gaming Mode. ARM needs its own build, load and input checks; x86 results do not count. Gaming Mode is a preview tested only on a virtual Deck. A new Plasma or Qt version, distribution or architecture needs build, load and real-input checks before it is advertised. A new language needs a matching dictionary, models and layout tests; changing a locale label alone adds no recognition. See the [current release scope](docs/releases/v0.5.0-1.md) and [release procedure](docs/releasing.md); before a release, complete `docs/release-acceptance.md`.

## Licensing

The project is REUSE-compliant (`reuse lint` must pass). Most code is **MIT**: engine, tools, scripts, benchmarks, launcher, Gaming Mode daemon, the shared language and session rules in `plugin/` (the `swipekb_language`, `swipekb_session` and `swipekb_swapflow` sources plus `nextwordworker` and `learningmanager`), their tests and every CMake file. Only the rest of `plugin/`, `frontend-plasma69/` (both plug into GPL code) and KDE's code in `scripts/patches/` use `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`. Documentation is CC-BY-4.0, trivial data CC0-1.0, and data files keep their own licences (see `REUSE.toml`).

New source files start with an SPDX header (`SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>` plus the licence of their folder); `tests/packaging/test_licensing.py` checks it. Anything copied from elsewhere keeps its original licence and credit, and Qt, KDE, dictionary and training-data projects must be credited.
