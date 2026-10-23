# Initial publication audit

Scope: the first public v0.1.0 test release. This records technical checks, not legal certification or a guarantee that a scanner detects every secret.

## History and source

Official Gitleaks v8.30.1 was downloaded from its GitHub release and verified against its published SHA-256. With redacted output:

- All available local/remote refs: 41 commits scanned, exit 0, no leaks found.
- Current staged first-party snapshots: exit 0, no leaks found. Publication documentation is scanned again before commit/push.
- History filename/blob inspection: 182 first-party blobs, no binary/NUL-containing blobs. No owner phone APK, decompiled source tree, typing traces, results/screenshots, phone UI dumps or production signing key was found committed.
- The optional historical `gboard-h2h` developer branch has a maintainer-local executable path, not an embedded phone serial or credential. Serial-literal checks found no candidates. Existing research prose describes static inspection; it is not presented as proof of rights beyond the licenses actually shipped.

## Complete corresponding-source archive

The archive includes recursive public upstream dependencies. Its full scan raised 25,302 generic-key matches in 98 vendor files:

- 15 source-code identifiers/expressions/signatures, not embedded credentials.
- 804 cpuinfo device-mock and boot-log test fixtures, not owner device data.
- 24,483 literal base64 tokenizer-vocabulary fields, not API credentials.

All 98 flagged files were byte-identical to blobs at their owning pinned public upstream commits. One bundled upstream test APK and one debug keystore were also byte-identical to Facebook buck2-prelude commit `48c249f8c7b99ff501d6e857754760315072b306`; neither is the owner's Google APK or production key. Do not strip upstream licenses/source merely to make generic scanner counts zero.

## Attribution and distribution

- GPL code is distinguished from FUTO Model Weights License 1.0 weights and the dictionary data (AOSP Apache-2.0 plus ODC-By/CC BY 2.0 FR word counts; no share-alike).
- Visible “Powered by FUTO Swipe” attribution is present in the keyboard selection desktop entry and documentation; model terms/notices are distributed.
- Binary files are accompanied by their complete frozen corresponding-source tarball and Fedora source RPM. GitHub's automatic source archives omit submodules, so they are not advertised as complete build source.
- Required model/build inputs are pinned and hash-checked. Public source changes after the verified build are publication documentation; executable/build/test inputs were compared against the frozen archive with zero differences across 78 files.
- No affiliation/endorsement by FUTO, KDE, Google or ASUS is claimed. The maintainer's YouCoded affiliation is disclosed without making it a dependency.

## Privacy boundary

VM images, private SSH keys/passwords, recording data, local scanner reports and test-runtime logs remain outside this Git repository. The included screenshot contains synthetic test text only. Reports shown to reviewers were redacted; no secret values are recorded here.
