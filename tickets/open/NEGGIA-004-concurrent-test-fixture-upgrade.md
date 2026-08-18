# Ticket: NEGGIA-004 — Concurrent-test fixture upgrade (real Eiger data, full worker coverage)

## Status
Open

## Type
infra

## Priority
P1 — Wave-1 enabler (amended tier-1 plan §Stages). Makes the concurrency gates of NEGGIA-005/006/007 meaningful.

## Origin
- Reporter: Max Burian (max.burian@dectris.com)
- Date: 2026-08-18
- Source: NEGGIA-001 closeout (AC-3 partial, deferral recorded in that ticket's Notes §Closeout): `Test_XdsPluginConcurrent` runs against the 5-frame, 11×13 px synthetic `dataset_artificial_small_001` fixture. With `frame % 16` dispatch, only workers 1–5 of 16 are ever exercised (worker 0 only via `plugin_get_header`); no real compression (bslz4/lz4) and no multi-data-file external-link path runs under concurrency. The TSan evidence from NEGGIA-001 is correspondingly narrow.

## Problem Statement
The one concurrency stress test in the suite exercises ~30% of the worker pool on toy data. NEGGIA-005 changes exactly the paths the test does not cover (per-worker caching of external-link data-file handles, real-compression decode under concurrent dispatch), and NEGGIA-006/007 add threads on top. Landing those against the current fixture would make TSan/Helgrind lanes green while leaving the actually-changed code paths unobserved — worse than no gate, because it *looks* like one.

## Expected Behavior
`Test_XdsPluginConcurrent` is parameterized over the real-detector fixtures `datasets_eiger1/` and `datasets_eiger2/` (real bslz4/lz4 compression + external-link multi-data-file layout), in addition to a synthetic fixture extended to ≥32 frames so every one of the 16 workers serves at least one frame under stress (32 ≥ 2×16; frame numbering is 1-based — off-by-one must not leave worker 0 cold). Bit-equality asserts against single-threaded reference reads are preserved for every fixture.

## Acceptance Criteria
- [ ] `Test_XdsPluginConcurrent` runs against `datasets_eiger1/` and `datasets_eiger2/` master files (compression + external links under 16-thread stress) with bit-equality asserts
- [ ] A synthetic fixture with ≥32 frames exists (extend `DatasetsFixture` generation or add a variant) and the stress test covers all 16 workers — verifiable by asserting that the set of `frame_number % 16` values exercised equals {0..15} (accounting for 1-based frames)
- [ ] ctest green on all lanes, including the NEGGIA-003 TSan/Helgrind lanes if merged first (soft ordering; whichever merges second gets the combined signal)
- [ ] Diff ≤ 50 cap units in test + fixture code (`test/` files; projection ~30)
- [ ] No production source touched (`plugin/`, `user/`, `data/`, `compression_algorithms/` byte-identical)

## Constraints
- **Touches:** `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp`, `src/dectris/neggia/test/DatasetsFixture.{h,cpp}`, `src/dectris/neggia/test/CMakeLists.txt`, `CHANGELOG.md`
- **ABI impact:** NO
- **Concurrency surface:** test-side threads only (existing pattern)
- **Coordinated XDS-fork update:** no
- **Reproducer:** run `ctest -R Test_XdsPluginConcurrent -V` and observe which workers serve frames (5-frame fixture → workers 1–5 only)

## 8-Step Workflow Log
1. **Ticket:** this file (created 2026-08-18)
2. **Requirement Spec:** `docs/specs/NEGGIA-004.md` (Wave 2)
3. **Audit & Challenge:** `docs/audits/NEGGIA-004.md` (light: test-only; audit checks the eiger fixtures' frame counts / dataset layout so the parameterization asserts the right totals)
4. **Minimal Patch Proposal:** ≤50 cap units or HALT
5. **Apply & Verify:** ctest + (if available) TSan/Helgrind lanes
6. **Commit/PR:** branch `feature/NEGGIA-004-fixture-upgrade` → master
7. **Changelog:** `[Unreleased]` → Added
8. **Learning:** optional

## Notes
- The eiger fixtures are 4-frame/2-data-file sets — they cover compression + external links but not full worker fan-out; that is what the ≥32-frame synthetic variant is for. Both are needed; neither alone suffices.
- NEGGIA-005 will change the dispatch key from frame-index to dataset-index (see amended plan §Stage design notes) — the worker-coverage assert must key off the *dispatch function*, not hardcode `% 16` semantics; the spec should define it via "every pool slot served ≥1 frame".
- Independent of NEGGIA-002/003 (different files); all three are Wave-1 parallel.
