# Releasing Swipe Keyboard

## Supported required set

`packaging/release-targets.json` is the authoritative version/release number and exact
required file list. Current corrective release: **0.2.1-1** (tag **v0.2.1-1**).
It requires Arch/CachyOS x86_64, Fedora **44** x86_64, Fedora **44 aarch64**
for Linux ARM64 Plasma Mobile, SteamOS **Desktop** x86_64 Flatpak, complete
corresponding source, one shared Fedora source RPM, Flatpak build inputs,
`release-manifest.json`, and `SHA256SUMS`. There are no platform skip switches.
ARM is a required build/package/acceptance target, not an optional canary.
Source implementation alone is not ARM runtime acceptance: the exact native ARM
package must pass a real ARM Plasma Mobile session before publication. This does
not certify physical-phone performance, Android, iOS, musl, or Gaming Mode.
Never replace v0.2.0 assets.

## One local check command

From a checkout with the recursively pinned submodules initialized and the pinned
small model metadata fixtures available:

```sh
python3 scripts/release.py check-versions && python3 -m unittest discover -s tests/packaging -v && python3 -m unittest discover -s tests/integration -v
```

`git submodule update --init --recursive` prepares the pinned source dependencies.
Normal complete asset preparation is `python3 scripts/fetch-assets.py` (CPython
3.14, Linux x86_64 or aarch64; preserves its existing 21 GiB free-space floor). Unit tests do
not compile all third-party products. Integration tests without built native helpers
can skip helper-dependent cases; the native package recipes build those helpers
and run their real tests. Source-only checks are not proof that a package works.

## PR, main, and manual checks

`.github/workflows/packages.yml` runs on every PR, pushes to `main`, matching tag
pushes, and explicit dispatch. No path filter can strand the required status. The
stable completion check is **Keyboard CI**; configure that exact observed successful
status as required, including administrators, only after a real green run exists.
Repository rule changes and the initial remote runs are owner coordination, not
something a local source test performs.

Required producer jobs are `prepare`, `arch`, `fedora`, `fedora-arm64`, and `flatpak`.
`keyboard-ci` (`Keyboard CI`) runs with `always()` and checks every producer result;
failed, cancelled, missing and unexpectedly skipped producers fail the gate.
Read-only credentials apply throughout PR/build/gate jobs. No `pull_request_target`,
cross-run artifact promotion, optional platform bypass, or product build cache exists.

- `prepare`: clean pinned checkout, recursive submodules, CPython 3.14, recipe
  consistency; fetches authenticated assets, builds language packs, captures licenses
  and native/SDK dependency wheel locks once. Emits a source identity inside the
  complete source archive. Python-only release/publication contract tests run from
  that archive; Qt/KF-dependent native suites run inside their actual distro jobs.
- `arch`: existing July 2026 Arch baseline recipe, unprivileged real build, tests,
  extracted installed-layout/ELF checks, ordinary-user installed wrapper checks,
  then repeats checks on the **identical** package bytes after updating the
  disposable container to current Arch. No dependency bypass.
- `fedora`: real Fedora 44 RPM/SRPM build, tests, extracted loader/ELF checks, and
  dependency-checked installation/removal in its disposable container.
- `fedora-arm64`: mandatory native `ubuntu-24.04-arm` runner with the multiarch
  Fedora 44 container, explicitly verifies `uname -m=aarch64` and actual free space.
  Builds fresh ExecuTorch/product objects with `USE_MARCH_NATIVE=OFF`, full native
  CTest and packaging/integration suites. Checks every shipped ELF machine,
  dependencies, privacy contracts, English/ES/FR/DE profiles and packaged QRC/loader;
  installs/checks/removes the exact RPM as for x86. Uploads only the `.aarch64.rpm`:
  the identical corresponding source recipe is already represented by the required
  shared source archive and x86-produced `.src.rpm`. Two host-dependent SRPMs with
  the same basename must never collide in merged downloads. The ARM producer's
  embedded source SHA/package version and RPM architecture are checked, not waived.
- `flatpak`: Ubuntu 26.04's signed distribution Flatpak tooling, checked to be
  1.16+ for inherited Wayland-socket support; no additional PPA is needed. Signed
  Flathub SDK/Platform 6.10 installation followed by exact locked
  commit selection and shared runtime verification. Builds its own objects in SDK
  Python 3.13/Qt, never reuses native distro objects. Tests installed `/app` loader,
  ELF imports and profile capability; installs the resulting exact bundle for
  embedded identity/ref checks. Default local `build.py` still requires the owned
  QEMU lab. Only the explicit `--ci-manifest` entry is used on hosted CI.

Flatpak preparation generates an **external** `manifest.json`, replaces the source
archive hash after archive creation, and materializes pinned upstream archives and
reviewed patches beside it. Dependency wheels are authenticated inside the complete
source archive rather than duplicated. The published `*-flatpak-build-inputs.tar.gz`
contains that portable recipe and its extra inputs; extract it beside the separately
published complete source archive (the recipe references `../swipe-keyboard-<version>.tar.gz`).
Its hash is not inserted back into the source archive. The tracked Flatpak manifest
is the template/legacy lab recipe, **not** a claim that its old local archive hash
identifies new source. Native and SDK wheel profiles remain separate. No signature
verification disable or SDK mismatch waiver is used.

Producer evidence binds the exact filename, architecture, embedded/package version,
source commit SHA, byte count and SHA256. The final gate rejects missing/duplicate/
unexpected evidence or outputs and verifies the directory's exact upload set.
Downloads default to **this workflow run**; producer status comes from `needs`, not
from uploaded claims. `verified-release` is the exact set to inspect/download.

## Corrective tag and publication

1. Review the PR, get actual complete remote `Keyboard CI` green for the candidate,
   merge through the required check, and identify the reviewed source SHA on `main`.
2. Tag that commit `v$(python3 scripts/release.py version)` and push the tag through
   the owner's normal release procedure. Both source and final publication checks
   require that **exact tag SHA** to be reachable from `origin/main`; it need not
   remain the latest main commit while builds run.
3. The tag runs **all producers again in the same workflow**. After `Keyboard CI`
   passes, the `publish` job waits for approval of the protected **keyboard-release**
   environment. Configure its required owner reviewer and disallow administrator
   bypass before using tag publication; a bare environment name alone is not protection.
   Download `verified-release` from this exact run, record required real-input checks
   against its package hashes, and only then approve publication. An earlier PR build
   is not evidence for newly rebuilt tag artifacts.
4. Only `publish`, after that approval, receives `contents: write`, only on this
   repository's tag push. It verifies tag/version/SHA and every transferred byte again, creates a draft,
   uploads the manifest-derived exact file list, downloads and checks the remote
   asset set/hashes, and only then makes the release public as a **prerelease**,
   preserving the current test-release channel. Stable-channel promotion requires
   an explicit policy change, not an accidental omitted flag. No rebuild or clobber.
5. A failure leaves a draft, not a partial public release. An existing draft or
   published release is refused on rerun; the owner must inspect/reconcile the draft
   explicitly before retrying. Existing published releases are never replaced.

For a manual nonpublishing remote run after pushing the reviewed branch:

```sh
gh workflow run packages.yml --ref ci/165-swipe-release-pipeline
```

Then inspect the actual run and logs (`gh run view <run-id> --log-failed`), not just
workflow syntax or local tests. Dispatch never publishes. Initial pipeline delivery
still needs real remote Arch/Fedora/SDK builds before it can be called working.

## Required ARM Plasma Mobile session acceptance

Before approving the tag run, install its hash-bound `.aarch64.rpm` into the owned
ARM Fedora 44 Plasma Mobile guest (or an explicitly authorized ARM device).
Record `uname -m`, OS/Qt/Plasma versions, source SHA, package version and SHA256;
verify native ELF/load/profile diagnostics and enable the keyboard using the
ordinary supported wrapper. In the real Plasma Mobile session, exercise taps,
swipe/commit, password/sensitive-field privacy, English plus ES/FR/DE profile
selection and the current native backend's prediction-disabled policy. Disable
and restore the exact prior keyboard configuration, then verify restoration and
package removal. Save real session screenshots/input results, not just OS boot
or a cross-compiled ELF. Owner environment approval must wait for this acceptance
against the final tag bytes, alongside the existing x86/Deck acceptance.

A QEMU-emulated ARM OS/session can establish bounded ARM Linux Plasma Mobile
build/load/input support, **not physical phone performance or device certification**.
Until native CI and that session have actually passed, ARM remains required but
unverified; it is not grounds to publish the other targets alone.

## Physical acceptance is separate

CI tests package, dependency, privacy/metadata and loader contracts, **not physical
compositor/touch input**. After owner-authorized installation, record the downloaded
package's SHA256 from the verified manifest, source SHA, version, target OS/compositor,
manual test steps and observed results. Native installation/configuration and rollback
are separate owner operations. Do not label a CI result as hash-bound physical
acceptance or infer a physical pass from an offscreen InputPanel.
