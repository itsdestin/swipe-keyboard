---
status: active
---
# Multilingual current-word and next-word prediction

## Scope
Enable genuine contextual next-word prediction for `es_ES`, `es_MX`, `fr_FR`, and `de_DE` while preserving the English model and the strict bridge/owned-history privacy gates. Spanish regions share one explicitly shared Spanish model. No UI changes, English fallback, or network access at runtime.

## Implementation sequence
1. Make candidate lexical validation Unicode-aware and pin Spanish/French/German spelling cases in focused engine tests.
2. Extend language-pack metadata with explicitly identified and integrity-pinned context model/vocabulary roles; keep unsupported/missing/corrupt packs incapable, and report next-word capability only when validated assets load.
3. Reuse the existing n-gram runtime and build pipeline for compact, reproducible per-language contextual models from a bounded, verified, legally compatible sentence corpus. Record source/license/version, hashes, sizes, attribution, and held-out contextual-vs-unigram quality.
4. Exercise profile/provider and native/bridge integration paths, slot ranking/selection, locale invalidation, and fail-closed behavior; retain all existing English and privacy-policy tests.

## Acceptance
Each supported locale can load its own model and produce context-sensitive candidates; switching locale cannot expose another language's candidates. Unicode spelling survives filtering. Missing/corrupt/unsupported assets fail closed. Native and raw bridge restrictions, completion acceptance semantics, and three-slot ranking remain unchanged. Run the focused tests during development and one combined production build/integration verification at completion; do not run CI or modify installed builds.
