# Pre-Reddit keyboard hardening

status: completed (local candidate; not published)

Results: see `docs/pre-reddit-verification.md` for exact package hashes, clean native builds, real KDE touchscreen acceptance and explicitly unrun cases.

## Approved outcome

The owner authorized the useful safety/setup changes identified in the read-only review, optional agent troubleshooting with a YouCoded link, and actual packaged installation/use/removal testing in the isolated Fedora KDE VM. No publication, commits or host keyboard changes are authorized.

## Design

Preserve the stock keyboard integration and native packaging. Bind an in-progress gesture to an input-context generation; cancel it on focus/reset, sensitive hints, input-method changes or layout geometry changes. Revalidate sensitive input in C++ before recognition, not merely recording. Candidate replacement must use the same selection/privacy safeguards as whole-word deletion. Bound gesture accumulation rather than redesigning recognition threading.

Keep activation reversible. Expand the read-only check output with non-content version/session/selection facts; do not collect typing or expose personal paths unnecessarily. Troubleshooting explains stock fallback, incompatible desktop upgrades, original-user disable, misordered uninstall recovery and optional AI-agent assistance (including https://youcoded.ai). Do not suggest bypassing package dependencies or deleting custom layouts blindly.

## Execution

1. Add focused failing behavioral tests for context invalidation, sensitive finish, active selections and interrupted geometry. Implement the minimal guards in `plugin/SwipeCapture.qml`, `plugin/swipeinputmethod.{h,cpp}` and relevant plugin tests. Run native tests against the guest-compatible build. Preserve ordinary tapping, candidate choices and one-shot whole-word backspace.
2. Add failing setup diagnostic tests in `tests/integration/test_setup.py`; implement a read-only report in `integration/setup.py` and version metadata in `cmake/Install.cmake` if needed. Update README, release-candidate notes and contributor guidance; preserve optional agent assistance.
3. Use an exclusively owned disposable VM session (not another session's VM maintenance work). Install the unchanged v0.1.0 package first if feasible, establish a real KDE touchscreen baseline and reproduce the risky user sequences. Build a clearly versioned local candidate from the changed source, install through the package manager and run normal-user check/enable.
4. In the actual Wayland session exercise touch activation, taps, sequential swipes, alternative selection, first/second backspace, selection safety, focus/password transition during a held gesture, cancellation/rotation or resizing, next-tap recovery, package reinstall/reboot and remove-before-disable recovery. Use guest-only uinput and real client fields; no direct plugin invocation counts as desktop acceptance. Inspect actual guest screenshots.
5. Build/test both native target packages if retained environments/disk permit; otherwise report the precise scope not verified. Preserve logs, package hashes and screenshots outside public source, shut down only this session's VM and retain the baseline unchanged.
6. Obtain a fresh code review, address findings, rerun affected checks and report exact passes/unrun cases. Never replace published v0.1.0 assets silently.

## Success criteria

A changed native package works through the normal install/enable/use/disable/remove path in a real KDE guest. Selection and focus/privacy transitions cannot authorize stale multi-character edits or stale swipe commits. Diagnostics remain read-only, recovery is demonstrated, optional agent help is accurately scoped, and verification claims distinguish real desktop checks from unit/load tests.
