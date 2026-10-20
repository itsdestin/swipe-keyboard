# Python executable build-input locks

Native locks target CPython 3.14, Linux glibc >= 2.28: the root lock is x86_64
(Fedora 44/current Arch); `native-aarch64/` is ARM64 (Fedora 44 Plasma Mobile).
The provisioner selects the native lock from `platform.machine()`, and rejects
other machines/interpreters before fetching. `kde-6.10/` remains a separate
CPython 3.13 x86_64 SDK lock; ARM does not reuse its extension wheels.

ARM CMake, PyYAML, msgpack, regex and wcwidth wheels are architecture-specific,
with exact URLs and SHA256 values checked against their pinned PyPI release
metadata. Pure Python wheels share existing pins. The pinned x86 torch wheel
is only a container for `torchgen/` Python/templates: no torch import, installation,
foreign native code or DSO adapter. Extraction rejects native/bytecode members.
The complete source producer includes all three authenticated wheel profiles.
Musl and other architectures are not covered.

`wheels.lock.json` records exact PyPI artifact URLs and SHA-256 hashes (checked
against PyPI release metadata), plus the PyTorch CPU wheel URL/hash from the
PyTorch CPU index. `futo.txt` and `dictionary.txt` are bounded pip requirements
with every version and wheel hash pinned, including transitive dependencies.
No source distributions, runtime dependency resolution, pip upgrades or full
torch installation are used during provisioning. Only `torchgen/` is extracted
from the verified torch wheel.

`scripts/provision-build-deps.py` fetches and verifies cached/downloaded bytes,
bootstraps pip from its verified wheel, then installs with `--require-hashes
--no-deps --no-index --only-binary=:all:`. Existing unmarked/stale venvs are
recreated; a content fingerprint marks complete provisioning. Download cache:
`$XDG_CACHE_HOME/swipe-keyboard/build-wheels` or `~/.cache/swipe-keyboard/build-wheels`.
Use `--cache PATH` to supply a prepopulated/offline wheel cache. No Git metadata
is needed, so the helper works in extracted source archives.

To update, resolve wheel-only dependencies in a disposable CPython 3.14 Linux
x86_64 environment, verify their SHA-256 values against release metadata,
update both the manifest and matching requirements, and rerun dependency tests,
torchgen-only import checks, native distro builds and deterministic dictionary
checksum verification. The wheel closure is deliberately fixed, not a promise
of perpetual compatibility with future Python or upstream dependency versions.
