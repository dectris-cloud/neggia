# Plan — Tier-1 performance overhaul

## Context

XDS-037 (RFC, transmitted 2026-05-06 — see `../xds/tickets/closed/XDS-037-neggia-parallel-loading-rfc.md`)
identified neggia's I/O path as the wall-clock bottleneck for
`xds_par` on object storage: ~40 s of every ~90 s run on a 192-core
node spent waiting on serial frame loads because the C++ plugin
funnels every read through a singleton + mmap, leaving 191 cores idle
during decode. The RFC proposed a "Tier-1" architecture that keeps
the four `plugin_*` C symbols unchanged (drop-in replacement; XDS-fork
needs only a `tools/neggia-version.txt` bump) but replaces the
internals: worker pool, pread, pre-decoded frame ring buffer. Modeled
speedup: ~1.6–1.8× wall-clock on the GeeseFS-S3 benchmark.

This plan is the operational decomposition of Tier-1 into 3 surgical
stages plus configuration + release tickets. Each stage is an
independent NEGGIA-NNN ticket through the full 8-step workflow,
gated by the framework's 50-line cap with HALT-on-overflow.

## Stages

### Stage 1 — `NEGGIA-001` — Worker pool replacing `GLOBAL_HANDLE` singleton

**Goal:** remove the structural pessimization at the plugin layer —
the unique-pointer singleton in `src/dectris/neggia/plugin/H5ToXds.cpp:31`
that forbids concurrent opens (lines 418-437) and gates every
`plugin_get_data` through one logical reader (line 475 → 143 lookup
without synchronization). Replace with a worker pool that gives each
worker its own state and exposes the parallelism XDS already has at
its own integration loop above the plugin.

**Surface (predicted):**
- `src/dectris/neggia/plugin/H5ToXds.cpp` — replace `GLOBAL_HANDLE`
  with a pool data structure; revise `plugin_open`/`plugin_get_data`
  to dispatch via the pool
- `src/dectris/neggia/plugin/H5ToXds.h` (or sibling header if needed)
- `src/dectris/neggia/test/Test_XdsPluginConcurrent.cpp` (NEW) —
  multi-threaded stress test (16 threads × 100 frames each)
- `src/dectris/neggia/test/CMakeLists.txt` — wire new test

**Hard predictions:**
- ABI: 4 `plugin_*` signatures unchanged (per spec invariant; surgical-
  patch HALTs otherwise).
- Cap: predicted 30-45 cap units (under 50 with header weight). If
  audit reveals header changes push above 50, re-scope into a
  sub-ticket "scaffold concurrent fixture before pool replacement".
- Concurrency gates: TSan + Helgrind on `Test_XdsPluginConcurrent`;
  bit-exact regression against pre-patch `.so` on
  `h5-testfiles/datasets_eiger{1,2}/`.

**Expected speedup at this stage:** ~1.3–1.5× wall-clock on the
GeeseFS-S3 benchmark. The singleton is the dominant structural
bottleneck; even keeping mmap, removing it surfaces existing
concurrency.

**Acceptance gates (spec will formalise):**
- `Test_XdsPluginConcurrent` passes baseline + TSan + Helgrind
- All 8 existing ctest cases still pass (bit-exact contract)
- `nm -D` shows exactly the 4 baseline symbols (ABI parity)
- (Manual, scientists-in-cloud) GeeseFS-S3 benchmark documented
  speedup ≥ 1.3×

---

### Stage 2 — `NEGGIA-002` — `pread` chunk-payload path replacing mmap

**Goal:** replace `mmap`-based chunk reads with explicit `pread` so
each worker drives its own request stream. `mmap` on FUSE generates
small page-faulted reads (kernel readahead window ~128 KB) which
GeeseFS turns into many tiny S3 GETs back-to-back; `pread` lets the
application issue one well-sized GET per chunk, multiple in flight.

**Surface (predicted):**
- `src/dectris/neggia/user/H5File.cpp` — add `pread` code path
  alongside mmap; route chunk-payload reads through it; keep mmap for
  master metadata regions where it's a win (per RFC §4.4)
- `src/dectris/neggia/user/H5File.h` — minor; add fd accessor or
  pread method
- `src/dectris/neggia/test/Test_PreadPath.cpp` (NEW) — verifies
  `pread`-read chunks bit-equal mmap-read chunks
- `src/dectris/neggia/test/CMakeLists.txt`

**Hard predictions:**
- ABI: unchanged.
- Cap: predicted 35-50 cap units. Closer to the cap than Stage 1
  because of dual-path support (mmap retained for master metadata).
- Concurrency gates: TSan + Helgrind on the new stress test
  (verify pread is thread-safe per POSIX in practice — fd shared
  across workers); bit-exact regression.
- Risk: kernel page-cache interaction between mmap'd metadata and
  pread'd payloads. Audit must walk through.

**Expected stage gain:** another ~1.3–1.6× over Stage 1 — the bulk
of the speedup. This is where the GeeseFS bandwidth limit moves from
~175 MB/s (single mmap stream) to 300–1000 MB/s (concurrent pread).

---

### Stage 3 — `NEGGIA-003` — Pre-decoded frame ring buffer with prefetch horizon

**Goal:** overlap decode and I/O. Workers fetch + decode chunks ahead
of the consumer's call site, depositing decoded frames into a ring
buffer. `plugin_get_data` becomes a memcpy from a slot rather than a
fetch+decode in the consumer's thread.

**Surface (predicted):**
- `src/dectris/neggia/plugin/H5ToXds.cpp` — ring buffer + prefetch
  horizon advance
- A new file `src/dectris/neggia/plugin/FrameRing.{h,cpp}` (NEW) for
  the ring data structure (≤25 lines source-counting)
- `src/dectris/neggia/test/Test_RingPrefetch.cpp` (NEW) — verifies
  ring correctness under under-flow (consumer faster than producer)
  and over-flow (producer outpacing consumer)
- `src/dectris/neggia/test/CMakeLists.txt`

**Hard predictions:**
- ABI: unchanged.
- Cap: predicted 35-50 cap units (FrameRing is a small RAII data
  structure; ring logic is mostly bookkeeping).
- Concurrency gates: TSan + Helgrind on prefetch and consume paths;
  bit-exact regression (decoded ring slots must match direct decode
  output).
- Memory budget: per RFC §4.3, default 16 slots × ~16 MB/frame on
  Eiger 4M = 256 MB; on Eiger 16M = 1 GB. Spec invariant: configurable
  via env var (`NEGGIA_PREFETCH_FRAMES`) with a sane default;
  documented in the README.

**Expected stage gain:** ~1.1–1.2× over Stage 2 (smaller but the final
piece — hides decode latency behind I/O; finishes Tier 1).

---

### Configuration — `NEGGIA-004` — Env-var configuration surface

**Goal:** expose worker count and prefetch frames via env vars per
RFC §4.2-4.3:
- `NEGGIA_WORKERS=<int>` — overrides the default
  `K = clamp(N_data_files × 4, 2, 16)`
- `NEGGIA_PREFETCH_FRAMES=<int>` — overrides the default
  `N = max(K × 2, 16)`

Add `README.md` entries documenting both, with examples for tight-memory
and high-bandwidth scenarios.

**Surface (predicted):**
- `src/dectris/neggia/plugin/H5ToXds.cpp` — getenv calls + clamping
- `README.md` — add a "Tunables" section
- `src/dectris/neggia/test/Test_EnvVarOverrides.cpp` (NEW) — verifies
  env-var parsing + clamp logic

**Cap:** small, predicted 10-15 cap units.

---

### Release — `NEGGIA-005` — Cut v1.3.0

**Goal:** tag a signed semver release once Stages 1-4 have closed.

**Steps:**
1. Update `CHANGELOG.md` `[Unreleased]` → `[1.3.0]` with date.
2. Bump version in `CMakeLists.txt` `project(neggia VERSION 1.3.0)`.
3. `git tag -s v1.3.0 -m "Tier-1 perf overhaul"`.
4. `git push origin v1.3.0`.
5. GitHub release published with CHANGELOG body.

**Scientists-in-cloud validation BEFORE the tag is signed:**
- Build v1.3.0 candidate locally.
- Run on 192-core node + GeeseFS-S3 against the canonical 3600-frame
  Eiger dataset.
- Compare wall-clock + frame-read throughput against pre-Tier-1
  baseline (XDS-037 measurement: 50 s compute / 40 s I/O / 90 s total).
- Acceptance: I/O time ≤ 10 s, wall-clock ≤ 60 s (combined ~1.5×).
- Record results in `CHANGELOG.md` and in
  `docs/learnings/NEGGIA-005-benchmark.md`.

**Downstream coordination — `XDS-042`** (back in xds repo): bump
`tools/neggia-version.txt` to the v1.3.0 commit SHA. PR through the
existing CI gate (XDS-039's ctest validation), run
`docs/release-validation.md` manually against your real dataset,
merge.

---

## Sequence dependencies

```
NEGGIA-001 ──┬─→ NEGGIA-002 ──→ NEGGIA-003 ──┬─→ NEGGIA-004 ──→ NEGGIA-005 ──→ XDS-042
             └─ (can start in parallel       ─┘   (small; can land
                if Stage 1 verifies clean)        before Stage 3 closes)
```

Stage 2 depends on Stage 1 (worker pool must exist before per-worker
pread fds make sense). Stage 3 depends on Stage 2 (ring buffer
prefetches from the pread path). NEGGIA-004 can land between Stage 3
close and Stage 5 release. XDS-042 is the downstream consumer bump
once v1.3.0 is signed.

## Risk register

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| TSan/Helgrind clean but real GeeseFS-S3 race | Low | High | Manual scientists-in-cloud validation before release sign |
| 50-line cap blows on Stage 2 (dual mmap+pread path) | Med | Low | Pre-scoped sub-ticket; minimal-patch HALT-on-overflow surfaces it cleanly |
| `pread` performance regression on local SSD (no FUSE) | Low | Med | Spec invariant: local-SSD baseline ≥ current; verify in ctest |
| ABI drift slips past audit | Very low | Critical | Hard-coded `nm -D` parity check in `neggia-verify`; sacred per Hard Rule 4 of `agent-framework.md` |
| Tier 1 ships but `xds_par` doesn't see speedup | Med | Med (sad day) | Manual XDS-fork-side validation before release tag; XDS-042 has its own validation gate |

## Tier-2 outlook (out of scope here)

The RFC's Tier 2 (`plugin_hint_frame_range` + `plugin_get_data_batch`)
is additive ABI extension. Not part of this plan. If Tier 1 speedup
falls short of projections, Tier 2 is the next lever and would land
as new NEGGIA-N tickets of type `abi-evolution` coordinating with
matching xds-side tickets.

## Realistic timeline (focused-session estimate)

- NEGGIA-001: 1-2 sessions (spec/audit/patch/verify cycle; cleanup may
  iterate once)
- NEGGIA-002: 1-2 sessions
- NEGGIA-003: 1 session
- NEGGIA-004: 1 session (small)
- NEGGIA-005 + XDS-042: 1 session each (release mechanics + downstream
  bump)

Total: 5-7 focused sessions across ~4-6 weeks calendar (allowing for
review cycles + scientists-in-cloud validation turnarounds between
Stage 3 close and release).
