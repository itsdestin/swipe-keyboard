# V1 candidate handoff

Preparation record for v0.1.0: final Arch/Fedora native candidates and package checks passed, including whole-word/candidate undo from the exact Fedora RPM in the full KDE VM. The owner subsequently approved publication. Current downloads/status: [v0.1.0 test release](https://github.com/itsdestin/swipe-keyboard/releases/tag/v0.1.0).

## Approved scope

Standalone English (US) swipe typing for KDE Plasma Keyboard, independent of ASUS/Z13, touch-profile and theming. Initial native targets: x86_64 current Arch/CachyOS and updated Fedora 44 KDE, Plasma Keyboard 6.7/6.8, package-matching Qt major/minor.

The larger LiquidGlass/exact Control Center/tablet layout/gestures/rotation setup remains deferred. Its agreed delivery is an agent-guided replication repo plus Destin's own recording/Reddit post, not a universal installer. Existing workspace work item #82 records both directions.

## V1 decisions

- The Firefox physical Ctrl+A/Delete context/leading-space quirk is accepted and documented as non-blocking. Stop the upstream investigation for this version.
- One ordinary Backspace immediately after a swipe or alternative choice undoes the word when exact context is available. It also removes only the separator introduced by that swipe. Typing, reported cursor movement, selections, password/sensitive input or mismatched/hidden context keep ordinary deletion.
- No forced multi-character deletion or global input monitoring to work around applications that omit/stale their field context.

## Verification already observed

- Local native suite: 15/15 CTest suites; setup/layout: 27 tests; packaging: 24 tests; guest-touch fixture: 6 tests, all passing.
- Actual stock InputPanel/Qt input engine tests include whole-word deletion, candidate undo, preserving typed spaces, ordinary deletion after typing, and cursor fallback. Fresh code review found no remaining blocker after an earlier-identical-word cursor regression was fixed.
- Full isolated Fedora KDE VM verified ordinary tapping, swipe words/spacing, candidate correction, symbols, password non-recording, owner-only opt-in recording, restoration, same-version reinstall, real reboot and Firefox Wayland typing. Whole-word backspace and candidate undo were additionally observed using the exact final native RPM in this VM; its disable/removal restored original settings and retained stock Plasma Keyboard. The conservative character fallback was also observed; no universal application-context guarantee is claimed.
- The final clean native package builds use a frozen source archive under `~/.cache/youcoded-z13-packaging/final-v1-input/` and write artifacts/logs under `final-v1-output/{arch,fedora44}/`. Do not mistake earlier candidates in `arch/exports` or `fedora44/exports` for these rebuilt files.
- A bounded privacy/attribution preflight found no concrete blocker: 109 indexed first-party files had zero high-confidence credential-pattern matches; recursive source archive names excluded private runtime/VM/key/trace material. This is not complete Git-history or legal certification.

## Boundaries

All runtime work stays in the dedicated VM or isolated roots. Destin's installed keyboard, normal KDE settings and running YouCoded app are untouched. VM images, passwords, keys and logs live outside the repository. Only exact task-owned VM/server processes may be stopped.

Final native artifacts, extracted-package checks and checksums are complete; see `docs/standalone-verification.md` and `final-v1-output/{README-FIRST.md,SHA256SUMS,release-manifest.json}`. The VM is shut down and synthetic touch input stopped. Publication was approved and the technical public-history/privacy/attribution review is recorded in `docs/publication-audit.md`. README/release notes provide the public download/source/issue URLs. No Reddit post is authorized here. No public AUR/COPR update repository exists yet; unsupported systems must not be advertised as tested.
