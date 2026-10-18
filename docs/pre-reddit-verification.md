# Pre-Reddit safety candidate verification

Status: local candidate acceptance complete; unpublished package release `0.1.0-2`. This report does not change the historical `v0.1.0` release's verification scope or downloads.

## Isolation and provenance

Testing used an exclusively owned reflink copy of the stopped Fedora 44 KDE acceptance VM. The acceptance copy is now shut down; its dedicated SSH listener is closed. The preserved baseline and maintainer's installed keyboard/settings are not modified. All touch/key input is directed only to this marked QEMU guest. Synthetic text only; private SSH/password files and screenshots remain outside corresponding source.

The public baseline RPM was independently matched to SHA-256 `492fc77112d6b42dde95d6e3372f64212d17ca4cc8fc5290277e26ba2f5ed8e0`. An older same-named retained candidate was identified by its different hash and excluded from published-release conclusions.

## Confirmed baseline behavior

- Exact public RPM: begin a touchscreen swipe in an ordinary Wayland text field, programmatically move the application's focus to a password field while the finger remains down, then release. Password length becomes 3 and word guesses appear. This is a reproduced stale-gesture/privacy bug, not evidence of general password recording.
- Exact public RPM: whole-word Backspace and undo after choosing an alternative work. An earlier contrary observation belonged to the superseded retained candidate.
- Ordinary native KWrite: touchscreen swipes produce `The world`; saving through the application's UI writes that text to its document.
- Misordered uninstall: native removal without disable leaves KDE pointing to the removed launcher and preserves the per-user restoration record. Reinstalling the exact public package then disabling as the original user restores both originally absent KWin keys exactly.
- Manual fallback: the actual KDE Virtual Keyboard settings offers **Plasma Keyboard**. Selecting it through the GUI saves the stock provider as the input method.

## Candidate acceptance

An independent host-native run passed all 15 CTest suites. Setup/overlay tests passed 28 cases, packaging tests passed 24 cases, and the guest-touch tooling safety suite passed 6 cases. The regression suite was observed failing before the selection guard (a selected field changed from `The` to `Thee`) and passing after hardening. A fresh source review found no confirmed regression, but specifically requires a real external Wayland client to validate `inputItem()` availability and ordinary swipe threshold/cancellation behavior.

Both clean distro builds passed (Fedora RPM: exit 0; Arch makepkg/container flow: exit 0). Each passed all 15 native suites, 28 setup/overlay tests and 24 packaging tests. Arch additionally passed extraction/payload validation, real stock InputPanel loading from the extracted package, ordinary-user installed `check` and native package removal. It ran in an isolated Arch container inside the disposable VM, not on the maintainer's installed keyboard.

The exact Fedora release-2 RPM was installed through `dnf`, checked and enabled as the ordinary KDE user, then exercised through guest-kernel touchscreen input and the real KWin input-method protocol:

| Check | Observed result |
|---|---|
| Ordinary swipe/correction/undo | `The` → first on-screen Backspace → empty; `The world` → candidate `The works` → Backspace → `The` |
| Focus-to-password during held swipe | Password length 0 (exact public baseline: 3); no candidate words shown in the sensitive field |
| Ordinary password interaction | Stock tapping retained; the tested drag over password keys produces one stock key character, not a recognized word. Two additional taps add exactly two characters |
| Backward selection into following text | Existing `The World` and its selected following text remain unchanged after touching the old alternative area |
| Display geometry interruption | Change actual output scale to 125% during a held swipe: no commit; restore 100%: next tap produces `H` |
| Long held gesture | Hold beyond the 10-second limit: no commit; next tap produces `H` |
| Default/opt-in recording | Recording switch initially absent and removed after testing; with explicit opt-in enabled, sensitive/interrupted gestures leave the existing trace file hash unchanged |
| Ordinary native KWrite | Touch activation and two swipes produce `The world`; save through the app UI writes that text to the document |
| Same-version reinstall/reboot | Native reinstall passes; real guest boot ID changes; package remains selected and real swipes/alternative undo work after reboot |
| Misordered uninstall recovery | Remove without disable, reinstall, disable: stock input-method path, originally absent visibility key and prior live mode are restored exactly |
| Custom-layout refusal/help | Synthetic existing overlay is preserved; installed error includes troubleshooting and optional YouCoded assistance links |
| Final removal | Disable then native removal leaves no package command/payload; stock keyboard process has no swipe environment or mapped swipe library; real stock taps produce `hi` |

One first post-reboot swipe fell back to single-character Backspace (`the` → `th`) while the newly opened field/keyboard synchronized context; a settled repeat passed whole-word and alternative undo. This is the conservative fallback, not silently treated as a passing whole-word assertion. Startup/context fallback is now explicit in the guide. Browser-specific acceptance was not rerun; the existing Firefox physical-clear spacing limitation remains accepted. Symbols, portrait rotation, interrupted multi-touch and physical-hardware behavior were not separately rerun in this candidate's full desktop sequence.

The review's two questions were settled by real external-client/KWrite swipes: `inputItem()` is available for this actual KWin proxy, and crossing the normal swipe threshold does not cancel every gesture.

## Frozen package hashes

| File | Bytes | SHA-256 |
|---|---:|---|
| `swipe-keyboard-0.1.0-2-x86_64.pkg.tar.zst` | 12402214 | `cec41d93995024382358da9ac9ebcb7780a53c915ec895503d5425edf8576c2a` |
| `swipe-keyboard-0.1.0-2.fc44.x86_64.rpm` | 11713675 | `8316eb3da5e0799a0227d58d9a1c0f4e6a30b9f29895870c84b30a6c998f9592` |
| `swipe-keyboard-0.1.0-2.fc44.src.rpm` | 189018985 | `0440ae5b66ba89451a3405d5f3e57e1c73f46e9362f0268b5e5de8fc51f1f8f5` |
| `swipe-keyboard-0.1.0.tar.gz` | 194413341 | `f33ed6c8467cc6639ad9f714d601ac44fbbb442881d990f95c3d3333e7240e6c` |

Runtime/setup/package-recipe files in the frozen source archive were independently compared byte-for-byte to the tested checkout. Later documentation-only edits do not change the compiled payload. Private verification logs and JSON assertions are retained under the dedicated hardening VM cache; no typing recordings are shipped.

## Distribution boundary

No candidate was published and no existing release assets were replaced. Arch native build/load/install checks are separate from Fedora's full desktop touchscreen acceptance; neither establishes arbitrary hardware, app, distro or Flatpak compatibility.
