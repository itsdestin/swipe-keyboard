# Contributing

Swipe Keyboard adds local swipe recognition to Plasma Keyboard. Contributions to recognition, language support, packaging, accessibility and documentation are welcome.

## Scope

See the [current release scope](docs/releases/v0.4.0-1.md) and [release procedure](docs/releasing.md). Required packages are Arch/CachyOS x86_64, Fedora 44 x86_64, Fedora 44 ARM64/aarch64 for Plasma Mobile, and Steam Deck Desktop Mode Flatpak, plus the SteamOS download for Gaming Mode. ARM must pass its own build/load and Mobile input checks; x86 tests are not ARM evidence. Gaming Mode is a preview tested only on a virtual Deck until the physical-Deck pass, and physical-phone performance and musl/other phone distributions require separate evidence. New Plasma/Qt versions, distributions and architectures need build/load and real-input checks before being advertised.

Keep recognition (`engine/`), Qt integration (`plugin/`), per-user activation (`integration/`) and distribution packaging (`packaging/`) separate. Do not add desktop layouts, tablet-mode policy or theme dependencies to the keyboard installer.

## Build and test

```sh
git clone https://github.com/itsdestin/swipe-keyboard.git
cd swipe-keyboard
python3 -m unittest discover -s tests/integration -v
```

All code is in this repository (no submodules); data files (word lists, n-gram packs, trained models) are downloaded by
checksum at build time (`scripts/fetch-assets.sh`). Release downloads also include a complete corresponding-source archive. Report bugs through [GitHub Issues](https://github.com/itsdestin/swipe-keyboard/issues); contributions to packaging, testing, documentation and language support are welcome.

Native build requirements and commands are defined in the target package recipes and build workflow, rather than a separate unmaintained dependency list. Build in an isolated distro environment. Source builds download build-time dependencies; binary-package users should not need a compiler, training corpus or Python environment setup. Do not use `-march=native` for redistributed binaries.

After a native build:

```sh
ctest --test-dir build --output-on-failure
```

To assess recognition changes, use the held-out swipe scoreboard (`bench/score.sh --label "description" --check`) and disclose dictionary/model/ranking configuration. Do not advertise top-three accuracy as first-guess accuracy or compare results from different datasets as equivalent. Optional ranking improvements must justify memory/latency costs as well as accuracy.

## Isolation and privacy

Tests use temporary HOME/XDG folders. Never enable, disable, restart or replace someone's running keyboard as part of automated unit tests. A container/plugin-load test is not a touchscreen test.

Recording is opt-in. Never attach `traces/`, learned words, typed context, credentials or unredacted logs to an issue or release. Tests needing private text should generate synthetic phrases.

## Changes and reports

Include the smallest reproducer, expected behavior, package version, distribution, Plasma Keyboard/Qt versions, Wayland/X11, locale and affected app. Include the hardware model when behavior depends on touch input.

Contributions beyond code are welcome: clean-install reports, other distro recipes, accessible interaction testing, language support, and documentation. A new language needs matching models/dictionary/layout tests; changing a locale label alone does not add recognition support.

Before a release, complete `docs/release-acceptance.md`. Give Qt/KDE, dictionary, training-data and optional model projects credit and preserve their licence terms.

## Licensing

The project is REUSE-compliant (`reuse lint` must pass) and "MIT where possible" (owner decision 2026-10-23): engine,
tools, scripts, benchmarks, launcher, Gaming Mode daemon, their tests and every CMake file `MIT`; only `plugin/`,
`frontend-plasma69/` (both plug into GPL code) and KDE's code in `scripts/patches/` use
`GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`; documentation `CC-BY-4.0`; trivial data `CC0-1.0`;
data files keep their own licences (see `REUSE.toml`). New source files start with an SPDX header
(`SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>` and the licence for their folder;
`tests/packaging/test_licensing.py` checks it). Anything copied from elsewhere keeps its original licence and attribution.
