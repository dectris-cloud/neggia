# Spec: NEGGIA-001 — Worker pool replacing GLOBAL_HANDLE singleton

## Goal
Remove the structural concurrency bottleneck at the neggia plugin layer.
The unique-pointer singleton `GLOBAL_HANDLE` at
`src/dectris/neggia/plugin/H5ToXds.cpp:31` forbids concurrent opens and
gates every `plugin_get_data` through one logical reader. Replacing it
with a worker pool (each worker owning its own state, or sharing under
mutex — audit decides) exposes the parallelism XDS already has at its
integration loop above the plugin. Stage 1 of the Tier-1 perf overhaul
per `docs/plans/tier-1-performance.md`. The four `plugin_*` C symbols
preserve their signatures (ABI parity invariant); XDS-fork's
`tools/neggia-version.txt` bump path stays clean. Source-line cap: ≤50
per `neggia-surgical-patch` rules (incl. 1.5× header weight).

## Affected Source

- `src/dectris/neggia/plugin/H5ToXds.cpp:31` — `GLOBAL_HANDLE` singleton
  declaration; removed and replaced with worker pool data structure.
- `src/dectris/neggia/plugin/H5ToXds.cpp:418-437` — `plugin_open`;
  currently refuses concurrent opens (line 423 check). Revised to
  initialise the worker pool; per-worker state allocation happens here.
- `src/dectris/neggia/plugin/H5ToXds.cpp:441-473` — `plugin_get_header`;
  dispatches via pool.
- `src/dectris/neggia/plugin/H5ToXds.cpp:475-500` (approx) —
  `plugin_get_data`; dispatches via pool with per-worker H5DataCache (or
  shared instance under mutex; audit decides).
- `src/dectris/neggia/plugin/H5ToXds.cpp` — `plugin_close`; tears down
  the worker pool cleanly.
- `src/dectris/neggia/plugin/H5ToXds.h` (or sibling header introducing
  the pool data structure) — minor declaration changes, header-weight
  cap applies.
- `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` — NEW; 16-thread
  stress test asserting bit-equality against single-threaded reference.
- `src/dectris/neggia/test/CMakeLists.txt` — wire new test (one
  `add_executable` + `add_test` block).
- `tools/regress_bitexact.sh` — NEW helper script for AC-6 (frame-by-frame
  byte-compare against pre-patch `.so`).

Read-only references (the contract surface — not modified):

- `docs/abi-baseline.txt` — the 4 sacred `plugin_*` symbols.
- `../xds/src/generic_data_plugin.f90:140-170` — XDS-fork's Fortran
  C-binding declarations for the 4 symbols; load-bearing for ABI parity
  invariant. Must continue to match.
- `src/dectris/neggia/user/H5File.cpp:22-37` — current mmap-based open
  path; **NOT changed in this ticket** (mmap → pread is NEGGIA-002 scope).
- `src/dectris/neggia/data/H5DataCache.{h,cpp}` (approx — actual paths
  resolved by audit) — the per-handle cache type that wraps a parsed
  HDF5 file; thread-safety of this class is OQ-1 below.

## Invariants

1. **ABI parity.** `nm -D build/src/dectris/neggia/plugin/dectris-neggia.so
   | grep -E '\b(plugin_open|plugin_close|plugin_get_header|plugin_get_data)\b'
   | sort` matches `docs/abi-baseline.txt` exactly. Four lines, alphabetical.
   Source of truth: `docs/abi-baseline.txt` + `Hard Rule 4` in
   `docs/architecture/agent-framework.md`. ABI HALT in
   `neggia-surgical-patch` triggers if violated.

2. **Source-line cap.** ≤ 50 cap units per `neggia-surgical-patch`
   counting rules (additions + removals in `src/dectris/neggia/`,
   non-blank non-comment, with 1.5× weight on `.h`/`.hpp` lines). Source
   of truth: `.claude/skills/neggia-surgical-patch/SKILL.md` source-line
   counting rules. HALT if exceeded.

3. **Thread-safety guarantee on `plugin_get_data`.** After this ticket,
   `plugin_get_data(frame_number, ...)` is data-race-free under
   concurrent calls from any number of threads. Each call returns the
   frame as if it were the only call. Formally: for any two threads
   T1, T2 calling `plugin_get_data` concurrently with possibly different
   `frame_number` arguments, the result for each call is byte-identical
   to the result the same call would return single-threaded. Source of
   truth: spec assumption + verified by Acceptance Test AT-4
   (`Test_XdsPluginConcurrent` under TSan) and AT-5 (under Helgrind).

4. **Synchronisation primitive set documented.** The patch + audit
   declare which sync primitives are introduced (`std::mutex`,
   `std::atomic`, `std::condition_variable`, lock-free patterns) AND
   the invariant each guards. No undocumented synchronisation. Audit
   OQ-2 resolves whether per-worker H5DataCache (no sync needed at the
   data layer) or shared H5DataCache with a mutex.

5. **Bit-exact data path.** For every fixture under
   `src/dectris/neggia/test/h5-testfiles/datasets_eiger1/` and
   `datasets_eiger2/`, the post-patch `.so`'s `plugin_get_data(N)`
   returns byte-identical output to the pre-patch `.so` for every
   frame N in [0, total_frames-1]. Source of truth: spec assumption +
   verified by AT-6.

6. **Existing ctest baseline preserved.** All 8 upstream tests pass
   post-patch: `Test_Dataset`, `Test_EigerData`, `Test_DifferentH5Ver`,
   `Test_H5DataspaceMsg`, `Test_H5FilterMsg`, `Test_H5ObjectHeader`,
   `Test_XdsPlugin`, `Test_XdsPluginWithData`. Verified by AT-2.

7. **No regression on local-SSD single-threaded read latency.** Single-
   threaded `plugin_get_data` post-patch is ≤ 1.1× pre-patch wall time
   on a local-SSD fixture (the manual benchmark — defensive: a worker
   pool introducing per-call dispatch overhead should not pessimise the
   single-threaded fast path). Source of truth: spec assumption;
   verified by AT-8 (manual benchmark).

8. **Open/close semantics preserved.** `plugin_open` followed by
   `plugin_close` cleanly tears down the pool. No worker thread
   outlives the close. No leaked file descriptors. Source of truth:
   spec assumption; verified by AT-2's existing `Test_XdsPlugin`
   open/close cycle.

## Acceptance Tests

1. **AT-1 — ABI parity** — type: bit-exact / nm-based assertion
   (CI step or `neggia-verify` host-side).
   - Setup: post-patch build at `build/src/dectris/neggia/plugin/dectris-neggia.so`.
   - Action: `nm -D <so> | grep -E '\b(plugin_open|plugin_close|plugin_get_header|plugin_get_data)\b' | awk '{print $3}' | sort > /tmp/actual-abi.txt; diff /tmp/actual-abi.txt docs/abi-baseline.txt`.
   - Expected: exit 0; no differences.

2. **AT-2 — Existing ctest baseline green** — type: ctest.
   - Setup: post-patch build; `cd build`.
   - Action: `ctest --output-on-failure`.
   - Expected: exit 0; ctest summary reports 9/9 tests passed (the
     existing 8 + the new `Test_XdsPluginConcurrent`).

3. **AT-3 — `Test_XdsPluginConcurrent` exists and passes** — type: ctest.
   - Setup: post-patch build; the new test file exists at
     `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` and is wired
     into `src/dectris/neggia/test/CMakeLists.txt`.
   - Action: `cd build && ctest -R Test_XdsPluginConcurrent --output-on-failure`.
   - Expected: exit 0. Test body: spawns 16 threads, each calling
     `plugin_get_data(frame_n)` for `frame_n` in [0, 99] in a random
     order; computes a reference by running the same calls
     single-threaded first; asserts every returned frame from every
     thread bit-equal to the reference for the same `frame_n`.

4. **AT-4 — TSan clean on `Test_XdsPluginConcurrent`** — type: TSan
   (sanitizer rebuild).
   - Setup: separate build dir;
     `cmake -B build-tsan -DCMAKE_BUILD_TYPE=Release
                          -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
                          -DCMAKE_C_FLAGS="-fsanitize=thread -g"`
     `cmake --build build-tsan --parallel`.
   - Action: `cd build-tsan && ctest -R Test_XdsPluginConcurrent --output-on-failure`.
   - Expected: exit 0; ZERO `WARNING: ThreadSanitizer` lines in the
     test log (`grep -c 'WARNING: ThreadSanitizer' ctest_output.log`
     returns 0).

5. **AT-5 — Helgrind clean on `Test_XdsPluginConcurrent`** — type:
   Helgrind.
   - Setup: post-patch build (or build-tsan; either works for Helgrind).
   - Action: `valgrind --tool=helgrind --error-exitcode=1
     ./build/src/dectris/neggia/test/Test_XdsPluginConcurrent`.
   - Expected: exit 0; "ERROR SUMMARY: 0 errors from 0 contexts" in
     Helgrind output.

6. **AT-6 — Bit-exact regression against pre-patch `.so`** — type:
   bit-exact (orchestrated by `tools/regress_bitexact.sh`).
   - Setup: pre-patch baseline `.so` produced via:
     `git stash; git checkout HEAD~1; cmake --build build-baseline; cp
     build-baseline/.../dectris-neggia.so /tmp/baseline.so; git checkout -;
     git stash pop`. Post-patch `.so` at
     `build/src/dectris/neggia/plugin/dectris-neggia.so`.
   - Action: `tools/regress_bitexact.sh /tmp/baseline.so
     build/src/dectris/neggia/plugin/dectris-neggia.so
     src/dectris/neggia/test/h5-testfiles/datasets_eiger1
     src/dectris/neggia/test/h5-testfiles/datasets_eiger2`.
   - Expected: exit 0; the script reports for each frame index N in
     each fixture: `byte-identical` (and exits non-zero on first
     mismatch). For Eiger1 fixture: ~5 frames; for Eiger2: ~5 frames.

7. **AT-7 — Source-line cap** — type: line-count check (host-side).
   - Setup: working tree at the patch commit.
   - Action: `git diff HEAD~1 -- src/dectris/neggia/ | awk '
     /^diff/ { in_h = ($0 ~ /\.h(pp)?$/) } 
     /^[+-][^+-]/ && !/^[+-]\s*$/ && !/^[+-]\s*\/\// {
       w = (in_h ? 1.5 : 1.0); s += w }
     END { printf "%.1f\n", s }
     '`.
   - Expected: numeric output ≤ 50.0.

8. **AT-8 — Benchmark threshold** — type: benchmark (manual,
   scientists-in-cloud, NOT CI-gated).
   - Setup: 192-core node + GeeseFS-S3 mount; 3600-frame Eiger dataset
     (~1 master + 2 data files); post-patch `xds_par` linked against
     post-patch neggia `.so` via `LIB=` in `XDS.INP`.
   - Action: run `xds_par` with `JOB= XYCORR INIT COLSPOT IDXREF DEFPIX
     INTEGRATE CORRECT` against the benchmark dataset; capture
     wall-clock from the XDS log's "Total elapsed wall-clock time for XDS"
     line.
   - Expected: wall-clock ≤ 70 s (vs the XDS-037 baseline measurement
     of ~90 s; ≥1.3× speedup). Defensive secondary check: local-SSD
     single-threaded read of the same fixture is within 1.1× of pre-
     patch (invariant 7; AT-8b in the closing report). Record results in
     `docs/learnings/NEGGIA-001.md` if the spec invariants 1-6 all pass
     AND the benchmark target is met.

## Out of Scope

- **mmap → pread migration.** `src/dectris/neggia/user/H5File.cpp:22-37`
  retains its mmap-based open path. Switching chunk payloads to pread
  is **Stage 2 (NEGGIA-002)** per the Tier-1 plan.
- **Pre-decoded frame ring buffer.** Each worker fetches+decodes
  synchronously in this ticket. Ring-buffer prefetch is **Stage 3
  (NEGGIA-003)**.
- **Env-var configuration surface.** Worker count uses the default
  formula `K = clamp(N_data_files × 4, 2, 16)` per RFC §4.2; env-var
  override (`NEGGIA_WORKERS`) is **NEGGIA-004** scope.
- **Tier-2 ABI extensions** (`plugin_hint_frame_range`,
  `plugin_get_data_batch`). Out of Tier 1 entirely; would be
  `abi-evolution`-type tickets with coordinated xds updates.
- **Changes to `H5DataCache` internal locking** if audit OQ-2 reveals
  it's already thread-safe enough for the per-worker pattern — no
  changes inside `data/` subtree. If audit reveals locking is needed
  inside `H5DataCache`, re-scope: NEGGIA-001 becomes pool-only, a new
  NEGGIA-001b covers `H5DataCache` lock surface.
- **GeeseFS-specific tuning.** Worker count clamps to 16 by RFC default;
  tuning beyond that (for higher-bandwidth links) is out of scope.

## Open Questions

These are resolved by the audit (step 3) before the patch is drafted.

- **OQ-1 — Is `H5DataCache` thread-safe internally?** The audit
  archaeologist must read `src/dectris/neggia/data/H5DataCache.{h,cpp}`
  (or its equivalent — discover via grep) and determine: does it hold
  any mutable state that's modified during a `getData` call (cursor,
  errno-style last-error fields, cached chunk index)? If yes,
  per-worker ownership is mandatory (each worker gets its own
  H5DataCache, opening the master file independently). If no (all
  mutable state is captured per-call on the stack), a single shared
  H5DataCache under a mutex suffices.
- **OQ-2 — Default worker count source.** Spec uses RFC's formula
  `K = clamp(N_data_files × 4, 2, 16)`. Audit confirms: where is
  `N_data_files` queryable from at `plugin_open` time? (Likely the
  master file's `/entry/data` group's link count; needs verification
  against the existing `H5DataCache` constructor.)
- **OQ-3 — Minimal pool data structure.** Options: (a)
  `std::vector<std::unique_ptr<H5DataCache>>` of size K, with workers
  picking by `frame_number % K` (no mutex needed if H5DataCache is
  thread-safe per OQ-1); (b) `std::vector<std::unique_ptr<H5DataCache>>`
  + per-element mutex (audit picks if H5DataCache needs locking); (c) a
  shared `H5DataCache` + single mutex (only viable if H5DataCache is
  truly thread-safe AND contention is acceptable). Audit picks based
  on OQ-1's resolution + cap-units projection.
- **OQ-4 — Does removing `GLOBAL_HANDLE` break any single-open
  invariant currently relied on by `plugin_open`?** The current code
  refuses a second `plugin_open` while one is active (line 423).
  XDS-fork's caller at `../xds/src/generic_data_plugin.f90:254`
  `dlopen`s the `.so` once per `xds_par` process and calls
  `plugin_open` once per dataset. Audit verifies: is there any
  third-party caller that relies on the explicit "one open at a time"
  refusal? If so, the worker pool must preserve the refusal semantics
  while allowing internal concurrency (single logical handle externally,
  multiple workers internally).
- **OQ-5 — Per-worker fd or shared fd?** If H5File still owns one fd
  (today: `H5File.cpp:30` `open()` + line 34 `mmap`), per-worker
  H5DataCache will multiply fds. Linux ulimit `nofile` default is 1024;
  16 workers × small fd count per file should be safe. macOS default
  is 256. Audit verifies feasibility.
