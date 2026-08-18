# Audit: NEGGIA-001 — Worker pool replacing GLOBAL_HANDLE singleton

## Spec Reference
`docs/specs/NEGGIA-001.md`

## Note on audit dispatch
This audit was dispatched via `general-purpose` (not `neggia-archaeologist`)
because the agent type was added mid-session and isn't loaded into the
dispatcher until next session. Next NEGGIA-NNN ticket can dispatch
`neggia-archaeologist` directly. Findings are equivalent — same tools,
same prompt brief, just a different dispatcher route this once.

---

## 1. Symbol / File Inventory

- **`GLOBAL_HANDLE`** — `src/dectris/neggia/plugin/H5ToXds.cpp:31` —
  file-scope `std::unique_ptr<H5DataCache>` in anonymous namespace;
  the singleton holding the one open dataset's parsed-cache state.
- **`H5DataCache` (struct)** — `src/dectris/neggia/plugin/H5ToXds.cpp:18-29` —
  **defined inline as a POD-like struct inside the anonymous namespace
  of `H5ToXds.cpp`**. **There is NO separate `H5DataCache.{h,cpp}`**.
  Members: `filename`, `H5File h5File`, `dimx`, `dimy`, `datasize`,
  `nframesPerDataset`, `mask`, `xpixelSize`, `ypixelSize`,
  `masterFileOnly`. Role: holds parsed header metadata + pixel mask +
  mmap handle for one open dataset.
- **`plugin_open`** — `H5ToXds.cpp:418-439` — C ABI. Constructs a new
  `H5DataCache`, opens master file via `H5File` ctor (line 425), then
  checks `if (GLOBAL_HANDLE)` at **line 431** (spec/ticket cite "line
  423" — actual is 431). Refusal at 432: `"CAN ONLY OPEN ONE FILE AT
  A TIME"`. On success: `std::move` into `GLOBAL_HANDLE` (line 437).
- **`plugin_get_header`** — `H5ToXds.cpp:441-473` — C ABI. Calls
  `getPreopenedDataCache()` (line 451), then mutates `dataCache->*`
  via `setXPixelSize`/`setYPixelSize`/`setPixelMask`/
  `setNFramesPerDataset` (lines 452-457). **All H5DataCache mutation
  happens here in this single one-shot during the open/header phase.**
- **`plugin_get_data`** — `H5ToXds.cpp:475-491` — C ABI. Calls
  `getPreopenedDataCache()` (line 483), then `readDataset(frame_number,
  data_array, dataCache)` (line 484). `readDataset` (lines 381-402)
  reads `dataCache` fields only and writes only to caller-supplied
  `data_array`. **No mutation in this call chain.**
- **`plugin_close`** — `H5ToXds.cpp:493-495` — C ABI. `GLOBAL_HANDLE.reset()`.
  **Audit-surfaced bug (out-of-scope):** `error_flag` is never assigned.
- **`H5File`** — `src/dectris/neggia/user/H5File.{h,cpp}`. RAII mmap
  wrapper. Holds `std::shared_ptr<char> _fileAddress` with custom
  `UnMap` deleter capturing `fsize`. **fd is `close(fd)`'d at
  `H5File.cpp:37` immediately after `mmap` — H5File does NOT retain
  the fd**.
- **`Dataset`** — `src/dectris/neggia/user/Dataset.{h,cpp}`. `Dataset::read`
  is `const`; instances are constructed per-call inside `readDataset`
  (`H5ToXds.cpp:387`).
- **`getPreopenedDataCache()`** — `H5ToXds.cpp:142-148` — file-scope
  helper that reads `GLOBAL_HANDLE.get()` and throws if null. **The
  single read site of `GLOBAL_HANDLE` outside open/close.**

---

## 2. Threading Model

### GLOBAL_HANDLE
- **Current**: main-only; **post-NEGGIA-001**: removed entirely.
- Synchronization: NONE. Raw `std::unique_ptr` at file scope. No mutex,
  no atomic, no thread_local.
- Invariant: exactly one live `H5DataCache` between matched
  `plugin_open`/`plugin_close`; refusal at `H5ToXds.cpp:431-435`
  enforces single-open externally.

### H5DataCache
- **Current**: main-only. **Post-NEGGIA-001 (candidate a, recommended)**:
  per-worker ownership → each instance is thread-confined; NO cross-thread
  mutation.
- Synchronization: NONE needed under per-worker ownership.
- **Inv-A — load-bearing**: All mutating writes to `dataCache->*` happen
  inside `plugin_open` (lines 424-425) and `plugin_get_header` callees
  (lines 182, 184, 191, 193, 232-235, 240-282, 331-334, 349, 352).
  After header phase: **field-immutable**. `plugin_get_data` reads only.
- Source: `src/dectris/neggia/plugin/H5ToXds.cpp:18-29` (struct);
  comprehensive grep confirms 100% of `dataCache->` writes inside
  open/header phase.

### H5File
- **Current**: main-only. **Post-NEGGIA-001**: shareable across worker
  threads as a read-only handle.
- Synchronization: relies on `std::shared_ptr<char>` standard-guaranteed
  thread-safe refcounting + PROT_READ kernel enforcement.
- **Inv-B**: H5File is safely copyable across threads. mmap region is
  `PROT_READ | MAP_SHARED`; fd is `close()`'d at construction (line 37);
  sharing across N workers does NOT multiply fds OR mmaps (shared_ptr
  refcount keeps one underlying mmap).
- Source: `user/H5File.cpp:30` (PROT_READ mmap), `:37` (fd close),
  `user/H5File.h:17` (`std::shared_ptr<char> _fileAddress`).

### Dataset
- **Inv-C**: per-call stack-local; `Dataset::read()` is `const`. No
  mutable cross-call state at Dataset layer. Source: `user/Dataset.cpp:111`.

### plugin_* (the 4 C ABI symbols)
- **Current**: main-only. **Post-NEGGIA-001 target**: `plugin_get_data`
  data-race-free under N-thread concurrent calls; `plugin_open`/
  `plugin_close` remain single-threaded (called once at session
  start/end by XDS-fork via dlsym).
- Synchronization: pool initialization happens-before any get_data call
  (sequenced by Fortran caller: open → header → loop(get_data) → close).
  Per-worker dispatch by `frame_number % K` — no synchronization on
  the hot path.
- **Invariant**: the four C symbol signatures MUST NOT CHANGE
  (Hard Rule 4 of agent framework; baselined in `docs/abi-baseline.txt`).
- Source: `H5ToXds.cpp:418, 441, 475, 493`; `H5ToXds.h:17-35`.

---

## 3. Call Graph

For each plugin_* symbol — one-hop callers and callees:

| Symbol | Caller (external via dlsym) | Callees (in-tree) |
|---|---|---|
| `plugin_open` (`H5ToXds.cpp:418`) | XDS-fork `generic_data_plugin.f90:283` (dlsym), `:288` (c_f_procpointer), `:304` (call site) | `setInfoArray` (line 404), `printVersionInfo` (line 33), `H5File::H5File(string)` (`user/H5File.cpp:45`) |
| `plugin_get_header` (`H5ToXds.cpp:441`) | XDS-fork `generic_data_plugin.f90:275, :280, :362` | `getPreopenedDataCache` (line 142), `setXPixelSize` (line 179), `setYPixelSize` (line 188), `setPixelMask` (line 224), `getNumberOfImages` (line 297), `getNumberOfTriggers` (line 310), `setNFramesPerDataset` (line 346) |
| `plugin_get_data` (`H5ToXds.cpp:475`) | XDS-fork `generic_data_plugin.f90:267, :272, :406` | `getPreopenedDataCache` (line 142), `readDataset` (line 381) → `Dataset::Dataset` (`user/Dataset.cpp:23`) + `Dataset::read` (`user/Dataset.cpp:111`) |
| `plugin_close` (`H5ToXds.cpp:493`) | XDS-fork `generic_data_plugin.f90:291, :296, :438` | `unique_ptr<H5DataCache>::reset` → `H5DataCache` dtor → `H5File` dtor → `shared_ptr<char>` dtor → `UnMap` → `munmap` |

**The 4 `plugin_*` symbols have exactly ONE external caller — XDS-fork's
`generic_data_plugin.f90`.** No in-tree neggia callers. ABI changes (none
in this ticket) propagate only there.

---

## 4. ABI Surface Impact

Candidate change: replace `GLOBAL_HANDLE` with worker pool data
structure. Per-symbol assessment:

- `plugin_open` signature: **unchanged**. Body changes internally. **NO ABI impact.**
- `plugin_close` signature: **unchanged**. **NO ABI impact.**
- `plugin_get_header` signature: **unchanged**. **NO ABI impact.**
- `plugin_get_data` signature: **unchanged**. **NO ABI impact.**
- Exported symbols: only the 4 `plugin_*` symbols are inside `extern "C"`
  blocks (`H5ToXds.h:8`, `H5ToXds.cpp:416-497`). No new exports. The
  `docs/abi-baseline.txt` 4-line nm baseline will still match.
- **XDS-fork C-binding compatibility** (`generic_data_plugin.f90:140-170`):
  byte-for-byte match verified — `plugin_open`/`plugin_close`/
  `plugin_get_header`/`plugin_get_data` all match Fortran `bind(C)`
  signatures.

**Verdict: ABI HALT does NOT trigger.** The 4 sacred symbols stay
byte-identical; XDS-fork's `tools/neggia-version.txt` bump path will
work cleanly post-NEGGIA-001 + post-release.

---

## 5. Invariants

Spec invariants 1-8 restated + audit-surfaced additions:

1. **ABI parity** — `docs/abi-baseline.txt` matches post-patch.
2. **Source-line cap ≤ 50** — `neggia-surgical-patch` rule.
3. **Thread-safety of `plugin_get_data`** — TSan + Helgrind verified.
4. **Documented sync primitives** — patch + audit declare.
5. **Bit-exact data path** — frame-by-frame byte-equal vs pre-patch `.so`.
6. **8 existing ctests preserved**.
7. **No local-SSD single-threaded regression (≤1.1×)**.
8. **Open/close clean teardown**.

**Audit-surfaced additions:**

- **Inv-A — H5DataCache is write-once-then-immutable after `plugin_get_header`.**
  All mutations of `dataCache->*` happen during open/header phase
  (lines 424-425, 182, 184, 191, 193, 232-235, 240-282, 331-334, 349, 352).
  Zero writes in `plugin_get_data`'s call chain. **Load-bearing for OQ-1.**
- **Inv-B — H5File is safely copyable across threads** as a read-only
  shareable handle. PROT_READ mmap; fd closed at construction;
  `std::shared_ptr<char>` thread-safe refcount.
- **Inv-C — Dataset is per-call stack-local with `const` read**. No
  mutable cross-call state at Dataset layer.
- **Inv-D — Single-open external contract** is announced via the stderr
  message at `H5ToXds.cpp:432`. XDS-fork does NOT depend on second-open
  failure (calls `plugin_open` exactly once per `xds_par` — verified
  `generic_data_plugin.f90:304`).
- **Inv-E — `plugin_close`'s `error_flag` is uninitialised** (`H5ToXds.cpp:493-495`).
  Pre-existing bug; out-of-scope for NEGGIA-001 but flagged for
  separate ticket.

---

## 6. Risks

Per the three candidate worker-pool designs:

| Candidate | Risk | Rationale |
|---|---|---|
| **(a) `vector<unique_ptr<H5DataCache>>` sized K, per-worker ownership, no get_data mutex, dispatch by frame_number % K** | **Low-Medium** | Eliminates Inv-A dependency entirely. Per-worker thread-confined state → trivially TSan-clean and Helgrind-clean. H5File mmap shared via shared_ptr (Inv-B) → K workers share ONE mmap. Risk is Medium only because the concurrency surface is new + has no prior ctest coverage. AT-3/4/5/6 are the mitigation. **RECOMMENDED.** |
| **(b) shared `H5DataCache` + `std::mutex` on get_data** | Medium | Serialises every get_data → defeats the entire purpose of the ticket. Throughput target AT-8 (≥1.3×) almost certainly unachievable. |
| **(b') shared `H5DataCache` + NO mutex (relies on Inv-A)** | Medium | Data-race-free in current code but fragile: a future patch (e.g. NEGGIA-002 mmap→pread) that adds get_data-path mutation silently regresses. Helgrind may flag unsynchronised reads of the underlying raw pointer anyway. |
| **(c) hybrid — shared master, per-worker data file handles** | Medium-High | Effectively collapses into (a) when `H5File` is shared via `shared_ptr` (Inv-B). The "hybrid" framing adds complexity without benefit given Inv-B. |

**Overall risk for NEGGIA-001 with recommended candidate (a): Medium.**
Single-TU refactor with NO ABI impact, but introduces a previously-absent
concurrency surface. The AC battery (ABI parity + 8 baseline ctests +
new Test_XdsPluginConcurrent + TSan + Helgrind + bit-exact regression +
cap check) is well-calibrated.

---

## 7. Existing ctest Coverage on Surface

- **`Test_XdsPlugin`** at `src/dectris/neggia/test/Test_XdsPlugin.cpp:31-156`
  — single-threaded; exercises all 4 `plugin_*` via dlopen+dlsym;
  9 test bodies covering ABI presence, open/close, header values,
  full-frame correctness. Uses `TestDatasetArtificialSmall001` fixture.
- **`Test_XdsPluginWithData`** at `src/dectris/neggia/test/Test_XdsPluginWithData.cpp:34-150`
  — single-threaded; exercises against real Eiger1/Eiger2 fixtures
  (8 fixtures total; BSLZ4 / LZ4 / uint8 / uint32 variants).
- **NO existing test covers concurrent calls.** No `mutex`/`atomic`/
  `thread`/`pthread` references in `src/dectris/neggia/test/`,
  `src/dectris/neggia/plugin/`, `src/dectris/neggia/data/`, or
  `src/dectris/neggia/user/` (verified via grep).
- **`H5DataCache` concurrency coverage**: NONE — TU-private struct,
  zero direct test coverage; exercised only transitively.
- **CMakeLists pattern**: `src/dectris/neggia/test/CMakeLists.txt:58-79`
  for the two existing plugin tests. New `Test_XdsPluginConcurrent`
  clones that block + adds `pthread` link for `std::thread`.

---

## 8. Challenge Questions Answered

**Who else calls this?** Exactly ONE external consumer per symbol:
XDS-fork's `generic_data_plugin.f90` via `dlsym` at lines 267/275/283/291
(declarations) and 304/362/406/438 (call sites). No in-tree neggia
callers — the 4 `plugin_*` symbols ARE the C ABI implementation.

**What invariants would silently break?** Three:
1. Inv-A (H5DataCache write-once) — if a future patch adds get_data-path
   mutation under shared-cache (b'), silent corruption. Mitigated by
   recommended per-worker design (a).
2. Inv-D (single-open external contract) — if worker pool allows second
   `plugin_open` while one is active, the documented stderr message
   doesn't fire. XDS-fork doesn't depend on it, but preserve the
   contract for any hypothetical downstream tooling.
3. Bit-exact correctness — if `Dataset` were non-idempotent across
   workers. Verified `Dataset` is per-call stack-local (Inv-C) → safe.

**Race-condition path?** Under shared-cache no-mutex design (b'): the
load of `GLOBAL_HANDLE.get()` at `getPreopenedDataCache()` (line 143) is
a non-atomic raw pointer load — could be torn or reordered on weakly-
ordered architectures. But happens-before ordering of Fortran's
serial open-then-call discipline + dlsym synchronisation makes the
race not reachable in practice. **The race only becomes reachable if
get_header runs concurrently with get_data** — e.g. if future XDS
parallelised the open phase. Mitigation in recommended design (a):
per-worker ownership means no shared mutable state exists at all.

**ctest coverage needed?** New `Test_XdsPluginConcurrent`: spawns 16
threads each calling `plugin_get_data(frame_n)` for `frame_n in [0,99]`
in randomised order; computes single-threaded reference; asserts
every returned frame bit-equal to the reference for the same frame_n.

**Last touched?** `git log -1 -- src/dectris/neggia/plugin/H5ToXds.cpp`:
`268aed2 2021-03-24 10:20:24 +0100 fix overflow handling`. Pre-
Dectris-cloud transfer; no fork-side edits since baseline.

**ABI still matches `generic_data_plugin.f90:140-170`?** **YES.** Four-by-
four signature match verified in §4. Worker pool is pure internal
refactor; `extern "C"` boundary preserved.

---

## 9. OQ Resolutions (load-bearing)

### OQ-1 — Is `H5DataCache` thread-safe internally?

**RESOLVED: H5DataCache is effectively thread-safe under Inv-A (write-once-
then-immutable after `plugin_get_header`), BUT per-worker ownership is
the safer engineering choice.**

Evidence: comprehensive grep of `dataCache->` writes — all 100% inside
open/header phase. `plugin_get_data` reads only. See `H5ToXds.cpp` lines
424-425 (open), 182, 184, 191, 193, 232-235, 240-282, 331-334, 349, 352
(header). Per-call state in `readDataset` is stack-local. Dataset is
per-call. H5File mmap is PROT_READ.

**Recommendation: per-worker ownership** for three reasons:
1. Eliminates the subtle Inv-A dependency — a future patch that adds
   a write in the get_data path silently regresses under shared design.
2. Cheap: H5File mmap is shared via `std::shared_ptr<char>` (Inv-B),
   so K workers share ONE mmap of the master.
3. TSan-clean and Helgrind-clean trivially — no shared mutable state.

### OQ-2 — Default worker count source

**RESOLVED: N_data_files is NOT directly queryable from existing code.**

Current code only discriminates `masterFileOnly` (line 349/352) via
existence-probing `/entry/data/data_000001` vs `/entry/data/data`.

Three options for NEGGIA-001:
1. Probe sequentially (`data_000001`, `data_000002`, ...) until
   `Dataset` ctor throws `out_of_range`. ~5 cap units.
2. Enumerate via `H5LinkInfoMessage`/`H5BTreeVersion2`. ~10-15 cap units.
3. **Hardcode K=16 unconditionally for NEGGIA-001** (defer N_data_files
   probe to NEGGIA-004's env-var work). ~2 cap units. **RECOMMENDED**
   for minimal-patch HALT compliance.

The formula's purpose is "expose XDS's existing parallelism"; K=16 is
the hard upper bound anyway. Refinement is NEGGIA-004 territory.

### OQ-3 — Minimal pool data structure recommendation

**RESOLVED: candidate (a) — `std::vector<std::unique_ptr<H5DataCache>>`
of size K=16, per-worker ownership, dispatch by `frame_number % K`.**

Cap-units projection (1.5× weight on `.h`/`.hpp`, but no header changes
in this design — all changes are TU-private in `H5ToXds.cpp`):

| Change | Lines added | Notes |
|---|---|---|
| `GLOBAL_POOL` declaration | 1-2 | replaces line 31 |
| `getPreopenedDataCache(int frame_number)` rewrite | 3-4 | replaces lines 142-148 |
| `plugin_open` rewrite (replicate H5DataCache K times after header) | 6-10 | adjustments at lines 418-439 |
| `plugin_close` rewrite | 1-2 | replaces line 494 |
| `plugin_get_data` rewrite | 1 | line 483 change |
| `plugin_get_header` adjust (header on worker[0], then field-copy or shared master cache) | 5-8 | lines 441-473 |
| **Total** | **17-27** | well under 50 |

Test file `Test_XdsPluginConcurrent.cpp` (~80-100 lines) is NEW but
**NOT counted against the 50-cap** per `neggia-surgical-patch` rules
(test files are exempt). CMakeLists addition: ~6 lines, also framework-
exempt.

### OQ-4 — Does removing GLOBAL_HANDLE break single-open invariant?

**RESOLVED: preserve externally, drop internally.**

Evidence: stderr message at `H5ToXds.cpp:432` is a user-visible
behaviour. XDS-fork's `generic_data_plugin.f90:304` calls
`dll_plugin_open` exactly once per `xds_par` process (verified via grep
— declaration at 173, bind at 288, single call at 304).

Implementation in NEGGIA-001: keep `if (!GLOBAL_POOL.empty()) { stderr
<< "NEGGIA ERROR: CAN ONLY OPEN ONE FILE AT A TIME ..."; return; }` at
the start of new `plugin_open`. Single logical dataset open at a time
externally; multiple workers internally.

### OQ-5 — Per-worker fd or shared fd?

**RESOLVED: per-worker H5DataCache does NOT multiply persistent fds.**

`H5File::H5File` at `user/H5File.cpp:45` calls `mapFile` (lines 18-41):
- Line 22: `open(path, O_RDONLY)` — opens fd.
- Line 30: `mmap(...)`.
- **Line 37: `close(fd)`** — fd released BEFORE the mmap region is
  returned via shared_ptr. mmap region remains valid post-close on
  POSIX (kernel retains the mapping reference).

Fd budget for K=16 + master + 2 data files dataset:
- Persistent fds: **0** (all closed at construction).
- Peak transient fds during `plugin_open` + `plugin_get_header`:
  ~3-5 (master + a few data-file resolves).
- During `plugin_get_data`: 0-2 transient per call (Dataset ctor may
  open a data file via `H5File` then immediately close).

Linux nofile default 1024; macOS 256. **K=16 workers consume <50 fds
peak.** No `setrlimit` raise needed.

---

## 10. Last-touched history

```
$ git log -1 --format='%H %ai %s' -- src/dectris/neggia/plugin/H5ToXds.cpp
268aed2 2021-03-24 10:20:24 +0100 fix overflow handling
```

5 most recent touching `H5ToXds.cpp`:
```
268aed2 fix overflow handling                                  [2021-03-24]
40edc21 Support all integer types for pixel_mask
8f12674 Decouple Dataset and H5DataLayoutMsg
74ad84f plugin: support signed int for ntrigger and nimages
3e57c35 support dataset with missing ntrigger
```

All pre-Dectris-cloud transfer (2021 era). No fork-side edits since
baseline. `H5File.cpp` + `Dataset.cpp` last touched `ff23220 2021-03-25
14:27:54 +0100 add support for data layout version 4`.

---

## Audit-surfaced anomalies (flagged for separate tickets — NOT NEGGIA-001 scope)

1. **Dead code at `H5ToXds.cpp:414`** — `std::unique_ptr<H5DataCache>
   retVal(new H5DataCache);` declared at file scope outside the anonymous
   namespace. Never referenced. Cleanup ticket candidate.
2. **`plugin_close` never assigns `error_flag`** (`H5ToXds.cpp:493-495`).
   Latent bug; tests pass only because callers pre-zero the flag.
3. **Wasted allocation on `plugin_open` refusal path** (lines 422-438):
   H5DataCache + mmap are allocated BEFORE the `GLOBAL_HANDLE` check.
   On refusal, the allocation is destroyed at scope exit. Cosmetic
   inefficiency; not a bug.

---

## Devil's Advocate

**Strongest argument against the proposed approach** (recommended
candidate (a) per-worker ownership): **if NEGGIA-002 (mmap → pread)
reintroduces mutation in the get_data path, per-worker isolation
catches it but the bit-exact regression gate may not — pread can
return data that's not byte-identical to mmap'd data in pathological
GeeseFS-S3 scenarios (e.g. consistent-read ordering differs between
mmap's page-fault stream and pread's explicit-range stream).** The
local ctest gate uses local files, not S3, so the bit-exact regression
on `h5-testfiles/datasets_eiger{1,2}/` will pass on the local-SSD
path. The real risk surface is the scientists-in-cloud benchmark
(AT-8) — if it fails on GeeseFS, the failure mode is silent stale
reads, not a TSan/Helgrind report. **Mitigation**: NEGGIA-002's spec
must include a Helgrind run against a network filesystem mount (or
explicitly declare local-SSD-only verification, with scientists-in-
cloud as the gating signal).

**Second-order effect on bit-exactness**: the OS page cache + shared
mmap behaviour under K=16 workers reading from the same master file —
if one worker's read triggers readahead that another worker subsequently
hits, are the views guaranteed coherent? **Answer**: yes, within a single
process the page cache is coherent (Linux/macOS guarantee this; mmap
PROT_READ is a kernel-mediated view). No risk.

**What would make this audit wrong?** If `H5File` retains hidden mutable
state I missed — e.g. if its destructor side-effects on the mmap'd
region. Verified: `UnMap` deleter (`user/H5File.cpp` UnMap class) only
calls `munmap`; no writes. If the audit is wrong on Inv-A (some
write site in get_data path I missed): per-worker ownership saves us
even from that miss, because each worker's state is private and
mutations would not race across workers — they'd just appear in the
wrong worker's slot. The bit-exact regression gate (AT-6) would catch
that as a fixture mismatch.

---

## Audit Verdict

**Verdict: READY_FOR_PATCH** — conditional on:

(a) **Patch chooses candidate (a)**: `std::vector<std::unique_ptr<H5DataCache>>`
sized K, per-worker ownership, dispatch by `frame_number % K`, no
get_data-path mutex.

(b) **Worker count K=16 hardcoded** (defer N_data_files probe to
NEGGIA-004's env-var work). Cap-units savings keep total under 50.

(c) **Single-open external contract preserved** (`H5ToXds.cpp:432`
stderr message kept verbatim or equivalent).

(d) **Test_XdsPluginConcurrent.cpp** authored per spec AT-3 (16 threads
× 100 calls; randomised frame order; bit-equality against single-
threaded reference computed first).

(e) **The 3 audit-surfaced anomalies** (dead code at line 414;
`plugin_close` error_flag bug; wasted-allocation refusal path) are
**NOT touched in this ticket** — separate tickets if and when worth.

(f) **No header file changes** — pool data structure lives TU-private
in `H5ToXds.cpp` anonymous namespace alongside existing `H5DataCache`
struct. This avoids the 1.5× header weight; keeps cap-units low.

**Rationale**: Change is structurally sound, isolates the concurrency
surface to a single TU, preserves the 4-symbol ABI byte-identically,
and inherits the existing single-threaded test coverage as a regression
baseline (AT-2). The Devil's Advocate concern about mmap → pread
interaction is for NEGGIA-002's audit, not this one. The recommended
candidate (a) cleanly satisfies the spec's 8 invariants under the
projected 17-27 cap-units envelope.

**Scope check passes** with the recommended design. No `SCOPE_MISMATCH`.
ABI HALT does NOT trigger. Spec invariants 1-8 + audit-surfaced
invariants A-E remain unchanged; the implementation surface is the
file list in the spec's Affected Source plus the new test file.

**OQ resolutions:**
- OQ-1 — RESOLVED — H5DataCache thread-safe under Inv-A; per-worker ownership preferred.
- OQ-2 — RESOLVED — K=16 hardcoded in NEGGIA-001; N_data_files probe deferred to NEGGIA-004.
- OQ-3 — RESOLVED — candidate (a) `vector<unique_ptr<H5DataCache>>` per-worker; 17-27 cap units projected.
- OQ-4 — RESOLVED — preserve external single-open contract (stderr message at line 432).
- OQ-5 — RESOLVED — no persistent fd multiplication; H5File closes fd at construction; ~50 fd peak budget; no `setrlimit` needed.

---

## Minimal Patch Proposal (cap-exempt under reporter override)

**Source-line count: ~67 / 50** (sum of additions + removals).
Reporter-direct override authorized 2026-05-29 (see
`tickets/open/NEGGIA-001-worker-pool-replacing-global-handle.md` § Notes).
Precedent: XDS-036 (2026-05-05, 155 cap units / 3.1× cap). Cap reaffirmed
in force for all subsequent NEGGIA-NNN tickets.

### Cap accounting (informational; override authorized above)

Detailed cap accounting under sum rule:

| Hunk | Removed | Added | Sum |
|---|---|---|---|
| Pool decl (line 31): `unique_ptr<H5DataCache>` → `vector<unique_ptr<H5DataCache>>` + K constant | 1 | 2 | 3 |
| `getPreopenedDataCache` rewrite (lines 142-148) with frame_number dispatch | 7 | 6 | 13 |
| `plugin_open` rewrite (lines 422-438): single-instance → K-loop with single-open check moved to front | 17 | 18 | 35 |
| `plugin_get_header` (insert after line 457): header-field propagation loop with mask memcpy | 0 | 12 | 12 |
| `plugin_get_data` (line 483): `getPreopenedDataCache()` → `getPreopenedDataCache(*frame_number)` | 1 | 1 | 2 |
| `plugin_close` (line 494): `GLOBAL_HANDLE.reset()` → `GLOBAL_POOL.clear()` | 1 | 1 | 2 |
| **Total** | **27** | **40** | **67** |

(Test files exempt from cap; `Test_XdsPluginConcurrent.cpp` ~80 lines,
`test/CMakeLists.txt` ~6 lines, `tools/regress_bitexact.sh` ~30 lines all
fall outside the source-line counting universe.)

### Affected files

| Path | New/Edit | Authored | Notes |
|---|---|---|---|
| `src/dectris/neggia/plugin/H5ToXds.cpp` | EDIT | here | Replace `GLOBAL_HANDLE` with `GLOBAL_POOL` of `NUM_WORKERS=16` `H5DataCache` instances; rewrite `getPreopenedDataCache` with frame-index dispatch; rewrite `plugin_open` (K-loop, single-open contract preserved); add header propagation loop in `plugin_get_header`; update `plugin_get_data` dispatch by frame_number; `plugin_close` clears pool. Cap-exempt under override. |
| `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` | NEW | here | AT-3/AT-4/AT-5: 16 threads × 100 plugin_get_data calls with randomised frame numbers; assert bit-equality against single-threaded reference. Test-cap-exempt. |
| `src/dectris/neggia/test/CMakeLists.txt` | EDIT | here | Wire `Test_XdsPluginConcurrent` (DatasetsFixture link + dl + pthread + gtest). Framework-cap-exempt. |
| `tools/regress_bitexact.sh` | NEW +x | here | AT-6: inline-compiles a small `dlopen`+`plugin_*` runner; runs against baseline + candidate `.so`; byte-compares every frame of every fixture under given fixture dirs. Framework-cap-exempt. |
| `CHANGELOG.md` | EDIT | here | `[Unreleased] → Changed` entry. Framework-cap-exempt. |
| `tickets/open/NEGGIA-001-…md` § Notes | EDIT | here | Reporter-override authorization recorded. |
| `docs/audits/NEGGIA-001.md` | EDIT (this section) | here | Minimal Patch Proposal section. |

### Per-hunk justification (H5ToXds.cpp)

1. **Includes (`<cstring>` + `<vector>`)** — required by the new
   propagation loop's `std::memcpy` + the new `std::vector` pool.
2. **`GLOBAL_HANDLE` → `NUM_WORKERS` + `GLOBAL_POOL`** — Inv-1 (ABI
   parity) + spec Goal (worker pool replaces singleton).
3. **`getPreopenedDataCache(size_t frame_index = 0)`** — OQ-3 candidate
   (a) dispatch helper; default `frame_index=0` selects worker[0] (the
   master cache used by `plugin_get_header`).
4. **`plugin_open` rewrite** — OQ-2 (K=16 hardcoded), OQ-3 (per-worker
   ownership; each worker constructs its own `H5File` for per-worker
   mmap = per-worker kernel readahead state), OQ-4 (single-open
   external contract preserved by check at top + stderr message
   verbatim).
5. **Header propagation loop in `plugin_get_header`** — Inv-A
   (H5DataCache write-once-then-immutable after header) propagated to
   all workers via field-copy + mask `memcpy`. Each worker's `mask`
   is a fresh allocation owned by that worker; thread-confined.
6. **`plugin_get_data` dispatch** — spec Invariant 3 (thread-safety
   guarantee on `plugin_get_data` via per-worker ownership; lock-free
   hot path).
7. **`plugin_close` clears pool** — spec Invariant 8 (clean
   teardown).

### Verification plan

Local-host verification matrix (Linux/macOS) — orchestrated by
`neggia-verify` skill:

1. **Build (Release)**:
   ```bash
   cd /Users/max.burian/Documents/03_Programming_Projects/neggia
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --parallel
   ```
   Expect: exit 0; `build/src/dectris/neggia/plugin/dectris-neggia.so` exists.

2. **AT-1 — ABI parity**:
   ```bash
   nm -D build/src/dectris/neggia/plugin/dectris-neggia.so \
     | grep -E '\b(plugin_open|plugin_close|plugin_get_header|plugin_get_data)\b' \
     | awk '{print $3}' | sort > /tmp/actual-abi.txt
   diff /tmp/actual-abi.txt docs/abi-baseline.txt
   ```
   Expect: exit 0; no diff.

3. **AT-2 — 8 existing ctests + new Test_XdsPluginConcurrent (AT-3)**:
   ```bash
   cd build && ctest --output-on-failure
   ```
   Expect: 9/9 tests passed.

4. **AT-4 — TSan**:
   ```bash
   cmake -B build-tsan -DCMAKE_BUILD_TYPE=Release \
                       -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
                       -DCMAKE_C_FLAGS="-fsanitize=thread -g"
   cmake --build build-tsan --parallel
   cd build-tsan && ctest -R Test_XdsPluginConcurrent --output-on-failure
   ```
   Expect: exit 0; zero `WARNING: ThreadSanitizer` lines.

5. **AT-5 — Helgrind** (Linux only; valgrind unavailable on macos-14 ARM):
   ```bash
   valgrind --tool=helgrind --error-exitcode=1 \
     build/src/dectris/neggia/test/Test_XdsPluginConcurrent
   ```
   Expect: exit 0; "ERROR SUMMARY: 0 errors from 0 contexts".

6. **AT-6 — Bit-exact regression**:
   ```bash
   # Build pre-patch baseline (from HEAD~1 of feature branch — the
   # neggia bootstrap commit at master):
   git worktree add /tmp/neggia-baseline master
   cmake -S /tmp/neggia-baseline -B /tmp/neggia-baseline-build \
         -DCMAKE_BUILD_TYPE=Release
   cmake --build /tmp/neggia-baseline-build --parallel
   # Compare:
   ./tools/regress_bitexact.sh \
     /tmp/neggia-baseline-build/src/dectris/neggia/plugin/dectris-neggia.so \
     build/src/dectris/neggia/plugin/dectris-neggia.so \
     src/dectris/neggia/test/h5-testfiles/datasets_eiger1 \
     src/dectris/neggia/test/h5-testfiles/datasets_eiger2
   ```
   Expect: exit 0; "PASS: all fixtures byte-identical".

7. **AT-8 — scientists-in-cloud benchmark** (out of scope for local
   verify; recorded in release notes per NEGGIA-005 plan).

### Verdict
- **READY_FOR_HUMAN_APPLY** (cap-exempt under reporter override
  2026-05-29; precedent XDS-036).
