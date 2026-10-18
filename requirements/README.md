# Python executable build-input locks

These locks target CPython 3.14, Linux x86_64, glibc >= 2.28 (Fedora 44 and
current Arch). Other interpreters/architectures fail closed; regenerate and
review a separate wheel lock before supporting them.

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
