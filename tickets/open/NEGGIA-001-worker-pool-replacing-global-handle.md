# Ticket: NEGGIA-001 — Worker pool replacing GLOBAL_HANDLE singleton

## Status
Open

## Type
perf

## Priority
P1 — Stage 1 of Tier-1 perf overhaul (see
`docs/plans/tier-1-performance.md`). Highest-value single change in the
plan: removes the structural concurrency bottleneck at the plugin
layer, surfaces the parallelism XDS already has above the plugin.

## Origin
- Reporter: Max Burian (max.burian@dectris.com)
- Date: 2026-05-29
- Source: XDS-037 RFC §4 Tier-1 architecture — first concrete
  implementation ticket post-framework-bootstrap (NEGGIA-000). The
  RFC identified `GLOBAL_HANDLE` in
  `src/dectris/neggia/plugin/H5ToXds.cpp:31` as the structural
  pessimization that prevents any concurrency at the plugin layer.

## Problem Statement
neggia's plugin layer funnels every `plugin_get_data` call through a
single unique-pointer singleton:

- `src/dectris/neggia/plugin/H5ToXds.cpp:31`:
  `std::unique_ptr<H5DataCache> GLOBAL_HANDLE = nullptr;`
- `H5ToXds.cpp:418-437`: `plugin_open` explicitly refuses concurrent
  opens.
- `H5ToXds.cpp:475` + `:143`: `plugin_get_data` and `plugin_get_header`
  reach into `GLOBAL_HANDLE` without any `std::mutex`, `std::atomic`,
  or other synchronization. The library is correct only because the
  application happens to call it serially.

Downstream effect (measured in XDS-037): on a 192-core node + GeeseFS
S3 mount, `xds_par` spends ~40 s of every ~90 s wall-clock loading
HDF5 frames through this serial path. ~44% of runtime is I/O overhead
because the singleton refuses to expose concurrency even though
GeeseFS sustains 300–1000 MB/s with concurrent readers and XDS's
OpenMP region above the plugin is willing to issue parallel requests.

## Expected Behavior
After this ticket lands:

1. `GLOBAL_HANDLE` is replaced with a worker pool data structure (per
   RFC §4.1 sketch). Default worker count `K = clamp(N_data_files × 4,
   lower=2, upper=16)`. (Env-var override surface is NEGGIA-004's
   scope; NEGGIA-001 uses the default formula.)
2. Each worker owns its own `H5DataCache` instance (or, if the audit
   reveals `H5DataCache` is already thread-safe internally, a shared
   instance with worker-local cursors — to be resolved at audit time).
3. The four `plugin_*` C symbols retain their existing signatures.
   XDS-fork's `tools/neggia-version.txt` bump path stays clean.
4. A new ctest case `Test_XdsPluginConcurrent` exercises 16 threads
   each calling `plugin_get_data` 100 times against the
   `h5-testfiles/datasets_eiger1/` fixture. Bit-equality is checked
   against the single-threaded baseline.
5. TSan-clean and Helgrind-clean on the new test.
6. Bit-exact regression against the pre-patch `.so` on
   `h5-testfiles/datasets_eiger{1,2}/`.

Quantitative targets:

- Wall-clock speedup on GeeseFS-S3 benchmark (manual, scientists-in-
  cloud): ≥ 1.3× total wall-clock vs pre-Tier-1 baseline (RFC
  measurement: 90 s → ≤ 70 s expected).
- No regression on local-SSD throughput (defensive — pure single-stream
  pessimization not introduced).

## Acceptance Criteria
- [ ] **AC-1 — ABI parity.** `nm -D
      build/src/dectris/neggia/plugin/dectris-neggia.so | grep -E
      '\b(plugin_open|plugin_close|plugin_get_header|plugin_get_data)\b' |
      sort` matches `docs/abi-baseline.txt` exactly.
- [ ] **AC-2 — Existing ctest still green.** All 8 upstream tests
      (`Test_Dataset`, `Test_EigerData`, `Test_DifferentH5Ver`,
      `Test_H5DataspaceMsg`, `Test_H5FilterMsg`, `Test_H5ObjectHeader`,
      `Test_XdsPlugin`, `Test_XdsPluginWithData`) pass post-patch.
      Verified via `cd build && ctest --output-on-failure` exit 0.
- [ ] **AC-3 — New concurrent stress test.** `Test_XdsPluginConcurrent`
      exists at `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp`,
      is wired into `src/dectris/neggia/test/CMakeLists.txt`, runs 16
      threads × 100 `plugin_get_data` calls each against
      `h5-testfiles/datasets_eiger1/`, asserts bit-equality of each
      returned frame against the single-threaded reference.
- [ ] **AC-4 — TSan clean.** `cmake -B build-tsan
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` followed by
      `cd build-tsan && ctest --output-on-failure` shows zero
      `WARNING: ThreadSanitizer` lines in any test log; exit 0.
- [ ] **AC-5 — Helgrind clean.** `valgrind --tool=helgrind
      --error-exitcode=1 ./build/.../Test_XdsPluginConcurrent` exits 0.
- [ ] **AC-6 — Bit-exact regression.** For every fixture under
      `src/dectris/neggia/test/h5-testfiles/datasets_eiger{1,2}/`, the
      post-patch `.so` returns byte-identical frames to the pre-patch
      `.so` for `plugin_get_data(N)` across N in [0, total_frames-1].
      Verified via `tools/regress_bitexact.sh` (new helper).
- [ ] **AC-7 — Source-line cap.** ≤ 50 cap units per
      `neggia-surgical-patch` counting rules (incl. 1.5× header
      weight). HALT if exceeded.

## Constraints
- **Touches:**
  - `src/dectris/neggia/plugin/H5ToXds.cpp` — singleton → pool
  - `src/dectris/neggia/plugin/H5ToXds.h` (or new pool header, if
    minimal) — to be determined at audit
  - `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` (NEW)
  - `src/dectris/neggia/test/CMakeLists.txt` — wire new test
  - `tools/regress_bitexact.sh` (NEW) — helper for AC-6
  - `CHANGELOG.md` — `[Unreleased]` → `Changed` entry
- **ABI impact:** **NO.** Type is `perf`, not `abi-evolution`. If
  audit reveals any candidate that changes a `plugin_*` signature,
  re-scope (or split off into a separate `abi-evolution` ticket).
- **Concurrency surface:** **YES.** Threading state introduced; spec
  must declare thread-safety guarantee + sync primitives + stress test
  per `neggia-requirement-spec` Hard Rule 4.
- **Coordinated XDS-fork update needed:** no — ABI unchanged means no
  `tools/neggia-version.txt` bump on this ticket. The release-bundle
  bump comes later in NEGGIA-005 → XDS-042.
- **Reproducer:** for benchmark validation: 192-core node + GeeseFS-S3
  mount + 3600-frame Eiger dataset across 2 data files. (Manual,
  scientists-in-cloud — not part of the per-PR CI gate; recorded in
  release-validation evidence per `docs/plans/tier-1-performance.md`
  §NEGGIA-005.)

## 8-Step Workflow Log
1. **Ticket:** this file (created 2026-05-29).
2. **Requirement Spec:** `docs/specs/NEGGIA-001.md` (TODO —
   `neggia-requirement-spec`). Spec must declare: thread-safety
   guarantee on `plugin_get_data` (sequentially consistent across
   any thread interleaving); sync primitive (mutex / atomic / per-worker
   ownership — audit decides); `Test_XdsPluginConcurrent` exact shape;
   ABI parity invariant; bit-exact regression invariant.
3. **Audit & Challenge:** `docs/audits/NEGGIA-001.md` (TODO —
   `neggia-deep-audit` driving `neggia-archaeologist`). Audit must
   resolve: (a) is `H5DataCache` thread-safe internally or must each
   worker get its own?; (b) does removing `GLOBAL_HANDLE` change any
   single-open invariant currently relied on by `plugin_open`?; (c)
   what's the minimal pool data structure that fits in ≤ 50 cap units?
4. **Minimal Patch Proposal:** TODO — `neggia-surgical-patch`. ABI
   HALT must not trigger (signatures unchanged). 50-line cap must not
   blow.
5. **Apply & Verify:** TODO. PR-side CI on
   `feature/NEGGIA-001-worker-pool`. PASS = all 6 verifications green
   (build, ctest, ABI parity, TSan, Helgrind, bit-exact).
6. **Commit/PR:** TODO. Branch `feature/NEGGIA-001-worker-pool`.
   Commit subject `perf(NEGGIA-001): worker pool replacing
   GLOBAL_HANDLE singleton`.
7. **Changelog:** TODO — `[Unreleased] → Changed`. One entry
   describing the singleton-to-pool transition + ABI preservation +
   ctest additions.
8. **Learning:** Encouraged — first real exercise of the neggia
   framework; worth capturing any surprises in the C++ archaeology
   patterns vs Fortran (XDS-fork) precedent.

## Notes
- Audit Devil's Advocate prompt suggestions: "If H5DataCache holds
  any thread-affine state (e.g. errno-style last-error fields), the
  pool-of-shared approach breaks silently. What does the audit see
  in `H5DataCache.h`?" and "If TSan+Helgrind both pass but
  GeeseFS-S3 production sees a stale-cache read, what's the
  explanation?" (Hint: the page cache.)
- The RFC §4.4 split — keep mmap for master metadata, switch chunk
  payloads to pread — is **Stage 2 (NEGGIA-002)**. NEGGIA-001 retains
  mmap throughout; speedup at this stage is purely from exposing
  concurrency, not from the pread bandwidth gain.
- Memory budget: NEGGIA-001 does NOT introduce the ring buffer
  (NEGGIA-003 territory). The worker pool's per-worker state is
  small (one H5DataCache instance each + bookkeeping); no significant
  RSS increase predicted.
- Plan: `docs/plans/tier-1-performance.md`.
