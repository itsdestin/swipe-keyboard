# Compatibility-first packaging

status: package compatibility work completed; native artifacts accepted. Publication and separately authorized host migration proceed under the release task. Maintenance automation is deferred to follow-up #165.

Results: `docs/revision3-verification.md`. Exact final Arch archive passes the older baseline, newer runtime, current-device dependency check and isolated loading without an OS upgrade.

## User requirement

Users should not need a whole-system upgrade merely to test this keyboard. Broaden compatibility in the public package, not a one-off CachyOS build. The previous device migration and downloadable release remain on hold until the compatibility candidate is verified. No forced dependencies, wholesale OS upgrades or live host keyboard experiments.

## Evidence

- Release-2 Arch metadata stamps every library in recursive `ldd` output with its build-machine package version. It includes duplicate package floors and fakeroot's injected library. `pacman -T` rejects this device's KF 6.30, Qt 6.12 package revision 2 and several unrelated older libraries.
- The exact release-2 Arch payload nevertheless passed all stock InputPanel swipe/safety tests against this device's installed libraries, using private HOME/XDG/offscreen settings and no desktop connection.
- A real cross-minor probe of the Fedora Qt 6.11 payload against host Qt 6.12 fails: two generated `QQmlPrivate::AOTCompiledContext::mark` imports require `Qt_6.11_PRIVATE_API`. Source code uses public Qt targets, but generated QML AOT code introduces this private ABI dependency.
- Engine ELF version requirements include GLIBC_2.43; that is a real baseline constraint to retain until an older-toolchain build proves it can be reduced.

## Design

Prefer minimal native packages over bundling another Qt/KDE desktop stack. Keep genuine loader/ABI constraints, but replace accidental build-machine package floors. Remove QML AOT generation for the small shipped QML module so that its C++ plugin uses the public Qt ABI; prove the resulting older-Qt build on a newer runtime before relaxing exact-minor activation checks. Baseline compilation must use the oldest realistic supported Qt/libc toolchain, and newer-runtime testing must run the same bytes. Do not claim arbitrary older KDE, absent Plasma Keyboard, other desktops or architectures from these changes.

## Execution and ownership

1. Add failing packaging tests for duplicate/transitive floors and fakeroot contamination. Generate dependencies from direct ELF needs, use package ownership without arbitrary package-release floors, and retain actual GLIBC/Qt ABI minima. Add a package guard that rejects unexpected private-Qt imports. Preserve real requirements rather than using `--nodeps` or metadata-only force installation.
2. Add a failing check for private QML-generated ABI symbols, enable `NO_CACHEGEN`, rebuild native code and run all native tests. Build the Qt 6.11 variant in the retained Fedora guest, load the same plugin against Qt 6.12 in an isolated test, and verify the module's actual undefined symbol versions.
3. Extend setup's compatibility policy only after successful cross-minor evidence: same Qt major, runtime minor at least the build baseline, plus an actual library/plugin-load preflight where needed to catch missing ELF symbols. Keep meaningful diagnostics and existing restoration behavior. Tests must cover older-runtime rejection, newer-runtime acceptance, damaged/missing libraries and read-only failure.
4. Choose and test an older native Arch build baseline, rather than silently assuming a newest-Arch build is broad. If this cannot be established here, explicitly separate verified same-minor/older-system-library improvements from future baseline expansion. Do not release an advertised broad package on unverified assumptions.
5. Rebuild candidate packages with a distinct revision, run native install/load/removal and real KDE touchscreen acceptance, inspect device dependencies without changing its current setup, then resume the separately authorized publication/device migration only if compatible. Preserve previous packages and corresponding source as historical evidence.

## Verification

Use `tests/packaging`, `tests/integration`, CTest, `readelf`/`nm` on final ELF files, isolated `scripts/check-stock-layout.py` runs, real Fedora KDE VM input and `pacman -T` against the exact revised archive on the older host. No broad compatibility claim comes solely from metadata changes or an offscreen test.
