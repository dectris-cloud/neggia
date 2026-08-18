# Spec: NEGGIA-004 — Concurrent-test fixture upgrade

## Goal
Make the concurrency stress test observe the code that the Tier-1 tickets actually change. Today `Test_XdsPluginConcurrent` stresses a 5-frame, 11×13 px synthetic fixture: only pool slots 1–5 of 16 ever serve a frame, and no real compression (bslz4/lz4) or multi-data-file external-link resolution runs under threads. After this ticket the test is parameterized over the real Eiger fixtures (`datasets_eiger1/2`) and over a ≥32-frame synthetic case, with bit-equality asserts preserved, so NEGGIA-005/006/007's TSan/Helgrind gates observe the changed paths.

## Affected Source
- `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` — parameterization + worker-coverage assert
- `src/dectris/neggia/test/DatasetsFixture.{h,cpp}` — fixture plumbing (eiger paths; ≥32-frame synthetic source TBD by audit)
- `src/dectris/neggia/test/CMakeLists.txt` — wiring if new test targets/definitions are needed
- `CHANGELOG.md` — `[Unreleased]` → Added

## Invariants
1. **ABI untouched / zero production diff:** no file under `src/dectris/neggia/{plugin,user,data,compression_algorithms}/` changes — `git diff --name-only master... | grep -vE '^(src/dectris/neggia/test/|CHANGELOG|docs/)'` is empty; `nm -D` 4-symbol parity trivially preserved — source of truth: `docs/abi-baseline.txt`
2. **Worker coverage:** under the ≥32-frame case, the stress run exercises **every pool slot** — asserted via "each of the K workers serves ≥ 1 frame", expressed against the dispatch function's observable behavior (frame count ≥ 2K with contiguous 1-based frame numbers guarantees it for modulo dispatch), NOT by hardcoding `% 16` — source of truth: ticket Notes (NEGGIA-005 will change the dispatch key to dataset-index; the assert must survive that)
3. **Real-path coverage:** at least one parameterized case runs bslz4-compressed, external-link, multi-data-file data (`datasets_eiger1` or `2`) under ≥16 concurrent threads with bit-equality asserts against single-threaded reference reads
4. **Bit-equality preserved:** every parameterized case asserts byte-identical frames between concurrent and single-threaded reads (memcmp over nx·ny·nbytes)
5. **Thread-safety exercise contract:** test-side threads only (`std::thread`, existing pattern); the test remains data-race-free under TSan and Helgrind (NEGGIA-003 lanes) — the plugin's guarantee under test is: concurrent `plugin_get_data` calls, after single-threaded `plugin_open`+`plugin_get_header`, are data-race-free and bit-correct
6. **Cap:** test + fixture diff ≤ 50 cap units (projection ~30); test files are test-scope but this ticket declares the cap anyway to keep Wave-1 discipline uniform
7. **Suite integrity:** all pre-existing ctest cases pass unmodified; total ctest count grows only by the new parameterized instances

## Acceptance Tests
1. **Parameterized concurrent stress green** — type: ctest
   - Action: `ctest -R Test_XdsPluginConcurrent --output-on-failure`
   - Expected: exit 0; log shows the eiger1, eiger2, and ≥32-frame synthetic instances all ran
2. **Worker coverage assert active** — type: ctest
   - Action: same run
   - Expected: the ≥32-frame case's coverage assertion executes and passes (all K slots served ≥1 frame)
3. **TSan/Helgrind on upgraded fixtures** — type: TSan + Helgrind
   - Setup: NEGGIA-003 lanes merged (soft ordering — whichever merges second delivers this signal)
   - Expected: both lanes green with the parameterized test included
4. **Bit-exact regression unaffected** — type: bit-exact
   - Action: `tools/regress_bitexact.sh <baseline.so> <post-patch.so>` (both = master build; no production change)
   - Expected: exit 0 — trivially, since the `.so` is unchanged; guards against accidental production edits

## Out of Scope
- Any production source change (dispatch-key change to dataset-index is NEGGIA-005)
- New compression codecs or HDF5-format fixtures beyond what h5-testfiles already provides
- Benchmarking (NEGGIA-002) and CI lane definitions (NEGGIA-003)
- Extending `tools/regress_bitexact.sh`

## Open Questions
1. Does an existing fixture already provide ≥32 frames in one master (e.g. `TestDatasetArtificialLarge001` — 1000 datasets × how many frames)? If yes, parameterize over it; if no, decide generation route (extend h5-testfiles submodule — who owns that repo per `.gitmodules` — vs. a checked-in generator needing h5py in CI) — audit resolves with fixture inventory
2. How do the eiger-data tests obtain dataset paths today (compile definitions in test/CMakeLists.txt? runtime discovery?) — audit quotes the pattern to reuse
3. Is there an existing gtest value-parameterized (TEST_P/INSTANTIATE_TEST_SUITE_P) precedent in the suite, or is this the first? — audit checks; if first, keep the parameterization minimal (fixture-path array + loop is acceptable under C++11/gtest-vendored constraints)
