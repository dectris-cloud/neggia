# Audit: NEGGIA-004 — Concurrent-test fixture upgrade

## Spec Reference
../neggia/docs/specs/NEGGIA-004.md

## Symbol / File Inventory
(from neggia-archaeologist, 2026-08-18)

- `Test_XdsPluginConcurrent.cpp` (149 lines): `NUM_THREADS=16`, `CALLS_PER_THREAD=100` (:44-45); class derives from `TestDatasetArtificialSmall001` (:48) — hard-bound to the 5-frame synthetic master; frame count from `plugin_get_header`'s 6th out-param (:81-84); serial 1-based reference loop (:89-96); 16 threads × per-thread `std::mt19937(0xC0FFEE ^ t)` uniform draws over [1,total] (:106-129); bit-equality via `std::equal` over nx·ny int32 (:122-123); own `main()` (:145-149).
- `DatasetsFixture.h:18-22`: `WIDTH=11; HEIGHT=13; N_FRAMES_PER_DATASET=5`; Small001 = 1 dataset (5 frames); **Large001 = 1000 datasets → 5000 frames / 1000 external data files** (`DatasetsFixture.h:33-47`, dir verified: 1001 files).
- Eiger tests use **no fixture class** — hardcoded relative literals (`Test_XdsPluginWithData.cpp:97-151`) resolved through the build-dir symlink (`test/CMakeLists.txt:3-7`); only compile definition is the absolute `PATH_TO_XDS_PLUGIN` (:8). Eiger fixtures: 4 frames each; 2-datafile variants = 2 frames/file; eiger1 1030×1065 uint16, eiger2 1028×512 uint8/16/32; bslz4 + lz4.
- **No TEST_P precedent, and the vendored gtest is 1.9.0-dev (pre-1.10): `INSTANTIATE_TEST_SUITE_P` does not exist** — only deprecated `INSTANTIATE_TEST_CASE_P` (spec OQ3 resolved: use a fixture-path array/loop or per-case TEST_Fs, not parameterized-test macros).
- Dispatch on master (`H5ToXds.cpp:151-156, :514`): slot = raw **1-based** frame `% 16`; `plugin_get_header` pins worker 0 (:461).

## Threading Model
Test-side threads only (existing pattern). New interleaving the upgrade observes: two threads dispatched to the *same* worker slot concurrently calling `readDataset` on one immutable `H5DataCache` — per-call `Dataset` + per-call buffer (`H5ToXds.cpp:395-405`) → should stay race-free; with bslz4/lz4 eiger data the **decompression paths run under concurrency for the first time ever** (the 5-frame synthetic is effectively uncompressed).

## Call Graph
Test → dlopen'd four symbols (unchanged). No production symbol modified.

## ABI Surface Impact
**NONE** (test-only). ABI HALT does not trigger.

## Invariants
1. Zero production diff (spec inv 1) — everything under `test/`
2. **Worker-coverage math (spec OQ resolved + spec inv 2 sharpened):** with slot = 1-based-frame % 16, contiguous frames 1..16 are the minimum covering all 16 slots (slot 0 first served by frame 16). Under NEGGIA-005's planned dataset-index dispatch (`globalFrame / nframesPerDataset % K`), covering all 16 slots needs ≥16 datasets → **≥ K × N_FRAMES_PER_DATASET = 80 contiguous frames of Large001**. Decision: the Large001 case tests frames 1..N_test with **N_test = 80** — covers all 16 slots under BOTH dispatch keys (frames 1..80 mod 16 = all residues; datasets 0..15 = all slots), so the assert survives NEGGIA-005 unchanged. Expressed as an assertion on the tested frame range (N_test == 80, contiguous, 1-based), never a literal `% 16` (spec inv 2).
3. **Bounded Helgrind cost:** capping Large001 at 80 frames (not 5000) keeps the serial reference loop + stress phase small enough for NEGGIA-003's ≤15-min Helgrind budget — the per-frame Dataset re-parse anomaly (`docs/learnings/NEGGIA-001.md`, → NEGGIA-005) makes a 5000-frame loop needlessly expensive under valgrind
4. Stress-phase draws are uniform over [1, N_test]; with N_test=80 and 1600 draws, residue coverage is morally deterministic (P(miss) ≤ 16·(15/16)^1600 ≈ 6×10⁻⁴³) and the *reference loop* provides the deterministic guarantee regardless
5. Bit-equality asserts preserved per fixture (spec inv 4); reference = single-threaded serial reads of the same master
6. Large001 caveat: **no plugin-level test has ever opened it** — `number_of_frames == 5000` is expected from fixture arithmetic but unverified; the new case asserts `total_frames == 5000` before capping to 80 (turns the unknown into a checked fact)
7. **Spec erratum (inv 6):** test files are cap-EXEMPT per NEGGIA-001's recorded accounting ("test-cap-exempt", CMakeLists "framework-exempt"); the spec's self-imposed ≤50 declaration is withdrawn — the ~30-line projection stands as a guideline, and the patch may take the clean-refactor route (extract a `runStressOn(master, nTestFrames)` helper + 4 cases) rather than a duplication-minimizing contortion

## Risks
- **Medium → Low after invariant 2/3 decisions**: (i) dispatch-key survivability — closed by the 80-frame rule; (ii) Helgrind wall-time — closed by the cap; (iii) Large001 plugin-level behavior unverified — converted into an explicit assertion; (iv) eiger cases add ~4-frame stress domains (slots 1-4 only) — they are for real-compression concurrency, not coverage; the audit records that division of labor so nobody "fixes" it later.

## Existing ctest Coverage on Surface
`Test_XdsPluginConcurrent` (Small001 only, workers 1-5, no real compression); `Test_XdsPluginWithData` (eiger, single-threaded); `Test_EigerData` (dataset API, per-datafile dims — source of the 2-frames/file constants); `Test_Dataset` (Small001 + Large001 via C++ API, one frame/dataset). Gap being closed: no concurrent test on real compression, external links, or slots 0 and 6-15.

## Challenge Questions Answered
- ≥32-frame fixture exists? Yes — Large001, 5000 frames; no generation tooling needed; h5-testfiles submodule is owned by upstream `dectris` org (extending it = governance decision), so reusing Large001 also avoids a cross-org write (OQ1).
- Path plumbing? Relative literals + build-dir symlink; nothing to extend in CMake beyond (possibly) nothing at all (OQ2).
- Parameterization? No TEST_P (pre-1.10 gtest); helper + per-case TEST_F (OQ3).
- What interleaving is newly observed? Same-slot concurrent `readDataset` on real compressed multi-datafile data — precisely NEGGIA-005/007's future surface.

## Devil's Advocate
- **Strongest argument against:** capping Large001 at 80 frames means the test never exercises deep external-link fan-out (data files 17..1000) or late-frame paths, so a hypothetical bug appearing only past frame 80 (e.g., B-tree deep-node traversal under concurrency) stays invisible — the cap trades completeness for Helgrind budget.
- **Resolution:** the 80-frame window spans 16 external data files and every worker slot, which is the concurrency surface this ticket exists to observe; deep-fan-out correctness is *single-threaded* HDF5-parsing territory already covered by `Test_Dataset`'s Large001 pass over all 1000 datasets. If NEGGIA-005's cache changes fan-out behavior, its own verify step must extend the window — recorded here as a forward note for the NEGGIA-005 spec.
- **Second-order effects:** ctest wall-time grows (4 cases instead of 1, one with an 80-frame reference loop) — minutes at worst natively; the NEGGIA-003 interplay is handled by the cap. No production or ABI effect.
- **What would make this audit wrong:** if Large001's master declared nimages ≠ 5000 (invariant 6 converts this to a first-run assertion failure, not a silent wrong test); if the vendored gtest's TEST_F machinery behaved differently across the 4 added cases — implausible, same macro as today.

## Cap-Unit Projection (sum-counted)
Test files cap-exempt (NEGGIA-001 precedent; spec inv 6 erratum above). Indicative size for review: helper extraction + 4 cases ≈ 60-80 sum lines in `Test_XdsPluginConcurrent.cpp`, ~0-6 in `DatasetsFixture.h` (Large001 already exists; likely zero), 0 in CMakeLists (same target). No headers weighted.

## Audit Verdict
- **READY_FOR_PATCH**
- Rationale: scope matches the spec (test/ only); all 3 OQs resolved — the decisive finding is that Large001 already provides the ≥32-frame multi-datafile fixture, eliminating generation tooling and submodule governance entirely; the 80-frame rule makes the coverage assert survive NEGGIA-005's dispatch change and bounds Helgrind cost; the one spec erratum (cap-exemption for tests) is recorded.

## Minimal Patch Proposal

Source-line count: **cap-exempt** (all changes under `src/dectris/neggia/test/` — test additions encouraged, not penalised; NEGGIA-001 precedent). Indicative size: +121/−70 in `Test_XdsPluginConcurrent.cpp`; zero changes to `DatasetsFixture.*` (Large001 already existed) or `test/CMakeLists.txt` (same target). No headers.

### Diff
Full unified diff at **`docs/patches/NEGGIA-004.patch`** (validated `git apply --check` — clean). Human applies:
```
git apply docs/patches/NEGGIA-004.patch
```
Shape: the NEGGIA-001 stress body becomes the fixture member `runStressOn(masterPath, capFrames, expectedTotal)` (byte-preserved logic; three textual parameterizations: master path, `n_test` in place of `total_frames` for the reference size/dist, plus the cap/expected-total block after `ASSERT_GT(total_frames, 0)`); four `TEST_F` cases — original Small001, eiger1 bslz4 2-datafile, eiger2 bslz4 uint32 2-datafile, and Large001 capped at `16*5 = 80` frames with `expectedTotal=5000`.

### Per-hunk justification
1. Header note + `#include <string>` — documentation + helper signature hygiene
2. `runStressOn` helper — spec invariants 4 (bit-equality preserved verbatim), 2 (cap logic → contiguous 1..80 coverage under both dispatch keys), audit invariant 6 (`expectedTotal` pins Large001's never-before-verified plugin-level count)
3. Eiger TEST_Fs — spec invariant 3 (real bslz4 + external-link multi-datafile under ≥16 threads)
4. Large001 TEST_F — spec invariant 2 (worker coverage) + audit invariant 3 (80-frame Helgrind budget cap)

### Verification (executed 2026-08-18 in a scratch copy outside the repo — the repo's src/ untouched per skill rule 3)
- Build: clean compile, Release, master toolchain ✓
- ctest: `Test_XdsPluginConcurrent` → **4 tests ran, all passed, 2.6 s total** (Large001 cap keeps it cheap) ✓
- Large001 plugin-level `number_of_frames == 5000` — **empirically confirmed** (was audit invariant 6's unknown) ✓
- Bit-exact regression: no production change; `.so` byte-identical — trivially satisfied (skill rule 5's TSan/Helgrind/bit-exact trio applies to threading-state changes; this ticket only *observes*; the TSan+Helgrind signal lands via NEGGIA-003's lanes once both are merged)
- ABI: untouched ✓

### Verdict
- **READY_FOR_HUMAN_APPLY** (apply the patch on this branch, push — the PR's CI incl. the NEGGIA-003 lanes, if merged first, is the final gate)
