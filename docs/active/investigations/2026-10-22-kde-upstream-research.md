---
status: active
cards: 179
---

# KDE upstream research: swipe + prediction for plasma-keyboard (2026-10-22)

Web-only, read-only. Gaps are marked UNVERIFIED. Most facts come from the invent.kde.org GitLab API and raw files fetched 2026-10-22.

## 1. plasma-keyboard state

- Repo: [plasma/plasma-keyboard](https://invent.kde.org/plasma/plasma-keyboard), created 2024-06-21. Started from Aleix Pol's June 2024 proposal to replace Maliit with Qt Virtual Keyboard ([discuss.kde.org](https://discuss.kde.org/t/plasma-virtual-keyboard-feedback-needed/39008)). Part of the "We Care About Your Input" KDE Goal. Akademy 2025 had an Input BoF ([wiki](https://community.kde.org/Akademy/2025/Input_BoF_1)); a goals slide deck lists swipe support as planned ([slides](https://conf.kde.org/event/9/contributions/292/attachments/159/205/KDE-GOALS_slides-ALL.pdf)).
- **Big change: it no longer wraps Qt Virtual Keyboard.** [MR !109](https://invent.kde.org/plasma/plasma-keyboard/-/merge_requests/109) "Introduce new input engine and framework to replace Qt VKBD" (Devin Lin, merged 2026-10-15; !110 landed the engine code 2026-04-21). It adds a `/virtualkeyboard` library (C++ engine plus QML components) and layout packages in `/layouts`. CJK uses system libraries (libpinyin, libchewing, anthy, libhangul). The MR text lists "swipe typing, split layouts, resizing, emoji search, and presage-based prediction" as follow-ups. The README no longer mentions Qt VKBD plugins. Main README is the only documentation (no CONTRIBUTING.md; 404).
- Maintainer/activity: Devin Lin is the dominant author (all 9 MRs found by search; Aleix Pol authored the original CMake). Newest commit 2026-10-19 "layouts: Port over most layouts"; l10n bot commits daily. Only English, Chinese, Japanese, Korean were ported at !109 time; others are being ported now.
- Prediction hook already designed: text composers subclass `AbstractTextComposer` (`keyEvent`, `selectCandidate`, `replaceLastInput`, preedit bubble) and feed `WordCandidateListModel` / `KeyboardStrip.qml`. [Issue #77 "Presage based word prediction"](https://invent.kde.org/plasma/plasma-keyboard/-/issues/77) (Devin, 2026-04-22, label Feature, 0 comments) proposes a `PresageTextComposer` imported by layout packages. English uses `DirectTextComposer` (no prediction).
- Earlier: [!107](https://invent.kde.org/plasma/plasma-keyboard/-/merge_requests/107) "Always disable predictive text" (2026-04-05) turned off Qt VKBD's prediction.
- Other issues: [#62 delete by swiping from backspace](https://invent.kde.org/plasma/plasma-keyboard/-/issues/62), #72 text expansion, #84 own language keyboards without XKB, #79 CJK. No issue on swipe/gesture typing, spellcheck, Hunspell or ML found in the ~35 newest issues (#62-#97). UNVERIFIED: issues #1-#61 and the full MR list (I only searched MR titles for "predict").
- Older: [bug 265452](https://bugs.kde.org/show_bug.cgi?id=265452) (2011, OSK word completion, resolved unmaintained). Forum requests for autocomplete: Fr3nzy-029, 2025-09-05 ([thread p.3](https://discuss.kde.org/t/plasma-virtual-keyboard-feedback-needed/39008?page=3)); no developer replies seen. UNVERIFIED: plasma-devel list and Phabricator archives (no hits).
- Flatpak manifest still builds `qt6-virtualkeyboard` 6.11.2 ([.flatpak-manifest.json](https://invent.kde.org/plasma/plasma-keyboard/-/raw/master/.flatpak-manifest.json)); possibly stale, UNVERIFIED.

## 2. KDE and ML models / big data

- **digiKam** keeps all models out of git, "due to licensing and the heavy size of files", downloaded on demand from files.kde.org ([Gilles Caulier, 2024-03-25](https://mail.kde.org/pipermail/kde-community/2024q1/007996.html)). Today files.kde.org/digikam/ hosts `facesengine/` (yunet, dnnface...) and `llm/` with a 1.0 GB `qwen2.5-1.5b-instruct-q4_k_m.gguf`, README says source Hugging Face Qwen, licence Apache-2.0 ([listing](https://files.kde.org/digikam/llm/), [README](https://mirrors.xtom.com/kde-application/digikam/llm/README)). So a KDE-hosted llama.cpp-class (GGUF) model exists.
- **Kdenlive** downloads Whisper/VOSK models and Python deps at first use into `~/.local/share/kdenlive/`; Flatpak has trouble ([docs](https://docs.kdenlive.org/en/effects_and_filters/speech_to_text.html)). Model licences are not stated there.
- **Krita** (2024) asked which licence fits a model plus dataset ([Halla Rempt](https://mail.kde.org/pipermail/kde-community/2024q1/007997.html), [Volker Krause](https://mail.kde.org/pipermail/kde-community/2024q1/007998.html)). [Cornelius Schumacher, 2024-03-30](https://mail.kde.org/pipermail/kde-community/2024q1/008003.html): policy "can't be applied directly" to models; tentative CC0 for data, MIT for models, plus openness labelling. **No decision reached.**
- [Licensing Policy](https://community.kde.org/Policies/Licensing_Policy): never mentions models/AI. App source: `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`; non-copyrightable data CC0; media CC-BY-SA-4.0 with editable source; files must be in preferred modifiable form; REUSE 3.0 required.
- Not found: a KDE "binary blobs" policy page (the guessed URL 404'd), any KDE project shipping ONNX Runtime, Itinerary/Okular/Tesseract model handling. UNVERIFIED.

## 3. Dependency and process expectations

- plasma-keyboard needs Qt >= 6.11, KF6 >= 6.30 (CoreAddons, I18n, KCMUtils, Package, Config, Crash), PlasmaQuick, xkbcommon, Wayland ([CMakeLists](https://invent.kde.org/plasma/plasma-keyboard/-/raw/master/CMakeLists.txt)). Dependencies for CJK are optional system libs. No ML runtime today. I found no KDE guidance on acceptable ML runtimes; precedent is OpenCV DNN (digiKam) and llama.cpp-style GGUF (digiKam).
- CI ([.gitlab-ci.yml](https://invent.kde.org/plasma/plasma-keyboard/-/raw/master/.gitlab-ci.yml), [.kde-ci.yml](https://invent.kde.org/plasma/plasma-keyboard/-/raw/master/.kde-ci.yml)): sysadmin/ci-utilities templates for clang-format, qml-lint, reuse-lint, xml/yaml-lint, linux/alpine/freebsd Qt6, linux-qt6-next, flatpak. `require-passing-tests-on: @all`, `run-qmllint: true`. A new dependency must exist on all CI images (incl. Alpine and FreeBSD) or be optional.
- i18n: ki18n; `po/` is synced by the l10n bot; `virtualkeyboard/Messages.sh` exists.
- KDE LLM-contribution policy is in flux: Nate Graham's draft guidelines were opened 2026-09-19 and closed 2026-09-21 after pushback; no adopted policy ([GamingOnLinux](https://www.gamingonlinux.com/2026/09/kde-developers-attempts-at-creating-llm-guidelines-are-not-going-well/), [discuss thread](https://discuss.kde.org/t/sorry-to-bring-up-a-contentious-topic-kde-ai-llm-policy/46333)). Draft: contributor must understand their code, no "Assisted-by" tags, discourage LLM-written commit/MR text. Aleix Pol: won't accept being "DoS'd by LLM-generated MRs".

## 4. Licensing

- Qt Virtual Keyboard is `LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0` (per [Qt conanfile](https://code.qt.io/cgit/qt/qtvirtualkeyboard.git/tree/conanfile.py?h=6.4.3), [Arch package](https://archlinux.org/packages/extra/x86_64/qt6-virtualkeyboard/)). That previously forced GPLv3 on plasma-keyboard; the CMake options for sounds and vibration still say "requires GPLv3 compliance".
- Declared SPDX: new engine files `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL` (e.g. `abstracttextcomposer.h`, `Keyboard.qml`; copyright 2026 Devin Lin). CMakeLists is `BSD-2-Clause`; `**.json` is `GPL-2.0-or-later` via `REUSE.toml`; CI yml files CC0-1.0. `LICENSES/` still contains `LicenseRef-Qt-Commercial` and GPL-3.0 files. Repo-wide: not every file checked.
- Implication: with the Qt VKBD wrapper gone, new code can use the standard KDE app licence triple. Anything copied from Qt VKBD (e.g. its Hunspell/lipi bits) would pull in GPL-3 only.

## 5. Privacy and language packs

- Only public privacy remark found: alexp11223 (2025-09-07) says Onboard's autocomplete stores typed words in a text DB that "can e.g. leak the password" ([thread p.3](https://discuss.kde.org/t/plasma-virtual-keyboard-feedback-needed/39008?page=3)); no KDE reply. Maliit used Presage ([bug 265452](https://bugs.kde.org/show_bug.cgi?id=265452)). No KDE policy on keyboard learning/storage found. No discussion of per-language data packs found. UNVERIFIED (searches of bugs.kde.org beyond one bug and plasma-devel returned nothing).

## Implications for our design

DO:
- Implement as a `AbstractTextComposer` subclass plus candidate strip, matching the new `/virtualkeyboard` architecture; target current master, not Qt VKBD plugins.
- Frame prediction as the answer to open issue #77 and swipe as the follow-up named in !109; reference both in any MR.
- License new files `GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL`, with REUSE headers, clang-format, qmllint-clean QML, `i18n()` for all strings.
- Keep ML/data out of git: host weights/dictionaries separately (files.kde.org-style, as digiKam does) or download on first use with consent; keep the keyboard fully usable with no pack.
- Make the model runtime an optional CMake feature (off if missing) so Alpine/FreeBSD CI stays green; prefer a small, packagable dependency over a large one.
- Choose permissively licensed, openly trained weights/data (MIT/Apache/CC0) and record provenance, since KDE has no model policy yet.
- Keep personalization off by default or on-device only, skipping password/sensitive fields; document it.
- Write code by hand-understood standards and keep MR text human-written; expect scrutiny of AI-assisted contributions.

DON'T:
- Don't bundle large weights in the repo or the Flatpak without maintainer agreement.
- Don't depend on Qt VKBD, or copy its code (GPL-3 only).
- Don't assume Hunspell/Presage are available; neither is in the manifest or CMake today.
- Don't add new always-required dependencies or data-collection/telemetry.
- Don't build against an old Qt/KF6; floors are Qt 6.11 / KF6 6.30.
