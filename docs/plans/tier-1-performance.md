# Plan — Tier-1 performance overhaul

> **Amended 2026-08-18** (with NEGGIA-001 closeout; user-approved re-plan; recorded in xds CHANGELOG under XDS-072's wave). The original stage order (pool → pread → ring) rested on a premise NEGGIA-001's landing disproved — see `## Amendment record` below. New order: **benchmark harness → CI concurrency lanes + fixtures → per-worker dataset cache → ring prefetch → tunables → cloud campaign → (conditional) pread → release**. Ticket IDs NEGGIA-002..005 as used below supersede the pre-amendment assignments (002=pread, 003=ring, 004=env, 005=release); none of the old assignments were ever intaken, so no ticket artifact breaks. NEGGIA-001's spec/audit reference the old numbering in their out-of-scope lists — those are immutable historical records.

## Context

XDS-037 (RFC, transmitted 2026-05-06 — see `../xds/tickets/closed/XDS-037-neggia-parallel-loading-rfc.md`)
identified neggia's I/O path as the wall-clock bottleneck for
`xds_par` on object storage: ~40 s of every ~90 s run on a 192-core
node spent waiting on serial frame loads. The RFC proposed a "Tier-1"
architecture that keeps the four `plugin_*` C symbols unchanged
(drop-in replacement; XDS-fork needs only a `tools/neggia-version.txt`
bump). Modeled speedup: ~1.6–1.8× wall-clock on the GeeseFS-S3
benchmark. Target (unchanged): **I/O ≤ 10 s, wall-clock ≤ 60 s** from
the 40 s I/O / 90 s wall baseline.

## Amendment record (2026-08-18)

Three findings from NEGGIA-001's landing force the re-plan:

1. **The serial-caller premise.** XDS calls `plugin_get_data` one frame
   at a time (`xds/src/formlib/generic_getfrm.f90:99`, quoted by XDS-037
   itself). Within one process there is no caller concurrency for a
   passive pool to "surface". Stage 1's shipped artifact — 16 sharded
   `H5DataCache` instances dispatched by `frame % 16`, zero threads —
   is **scaffolding whose payoff arrives only when in-plugin prefetch
   threads exist**. Its original "~1.3–1.5× at this stage" claim is
   withdrawn; the enabler (ring + prefetch threads) moves earlier in
   spirit: everything now builds toward it.
2. **The unaddressed dominant cost.** Every `plugin_get_data` constructs
   a fresh `Dataset`; for external-link data files that is a fresh
   `open`+`mmap`+superblock parse+B-tree walk **per frame**
   (`user/Dataset.cpp:36,45`). On FUSE that is several metadata round
   trips × 3600 frames × every process. No pre-amendment stage touched
   it. A per-worker dataset cache is the largest *certain* serial-path
   lever and multiplies the ring's later value.
3. **Nothing is measurable.** No benchmark harness exists anywhere in
   the repo (only correctness tooling). Every per-stage speedup number
   in this plan was unfalsifiable. The harness is now the first ticket,
   and every perf PR attaches an A/B CSV.

Also corrected: implementation divergence from audit Inv-B (16
independent master mmaps in `plugin_open`, `H5ToXds.cpp:437-443` —
assigned to NEGGIA-005); Helgrind has never run (no CI lane; dev
machine is macOS arm64 — assigned to NEGGIA-003); the concurrent test
exercises a 5-frame 11×13 px synthetic fixture, workers 1–5 of 16 only
(assigned to NEGGIA-004); the downstream bump ticket was pre-named
"XDS-042", an ID long since consumed in the xds repo (now: next free
XDS ID at bump time); `xds/tools/neggia-version.txt` still pins the
pre-fork baseline `a81bdf8` — **XDS consumes nothing of this work until
NEGGIA-011 lands.**

## Stages (amended)

| # | Ticket | Title | Scope | Cap projection (sum-counted, headers ×1.5) | Contribution to 90 s → 60 s |
|---|---|---|---|---|---|
| 0 | — | NEGGIA-001 closeout (this amendment + learning + PR #28 merge) | docs, ticket bookkeeping | exempt | unblocks all; merged `.so` = frozen A/B baseline |
| 1 | NEGGIA-002 | Benchmark harness + cloud protocol | `tools/bench_frames.sh`, `docs/benchmarks/protocol.md` | ~110 — **Exception C request** (measurement tooling, whole-file review; fallback 2-way split) | 0 — makes every claim measurable |
| 1 | NEGGIA-003 | TSan + Helgrind CI lanes | `.github/workflows/main.yml` | ~40 | 0 — hard prerequisite for 006/007; retro-Helgrind for 001 |
| 1 | NEGGIA-004 | Concurrent-test fixture upgrade | `Test_XdsPluginConcurrent.cpp`, `DatasetsFixture.*`, test CMakeLists | ~30 | 0 — real bslz4 + multi-datafile + ≥32-frame coverage of all 16 workers |
| 2 | NEGGIA-005 | Per-worker Dataset cache + shared master `H5File` + micro-anomalies | `plugin/H5ToXds.cpp` (~28) + tests (~15) | ~45 | **biggest certain lever**: I/O 40 s → est. 20–28 s |
| 3 | NEGGIA-006 | FrameRing core (header-only, C++11 mutex+condvar) + unit test | `plugin/FrameRing.h`, `Test_RingPrefetch.cpp`, CMake | ~55 — HALT expected; pre-negotiated trim-or-exemption | 0 alone |
| 3 | NEGGIA-007 | Ring integration: prefetch threads after `plugin_get_header`; get_data rendezvous; join+drain in `plugin_close` | `plugin/H5ToXds.cpp` (~35) + concurrent-test extension | ~48 | the target-reaching ticket: visible I/O → est. ≤10 s |
| 3 | NEGGIA-008 | Env tunables `NEGGIA_WORKERS` / `NEGGIA_PREFETCH_FRAMES` + the K-formula 001 promised | `H5ToXds.cpp`, `README.md`, test | ~15 | 0 direct; mandatory before cloud run (Eiger-16M ring = 1 GB) |
| 4 | NEGGIA-009 | Cloud benchmark campaign | per `docs/benchmarks/protocol.md`; results → learning doc + CHANGELOG | exempt | the credible number; **decision gate for 010** |
| 5 | NEGGIA-010 | *(conditional)* pread chunk-payload path | `user/H5File.{h,cpp}` (retain the fd `mapFile` closes at `:37`), `Test_PreadPath.cpp` | ~45–50 | only if 009 misses ≤10 s I/O |
| 5 | NEGGIA-011 | Release v1.3.0 + downstream bump | `CHANGELOG.md`, `CMakeLists.txt`; xds repo: new ticket (next free XDS ID) bumping `tools/neggia-version.txt` | ~3 | ships it |

### Stage design notes

- **NEGGIA-002 (harness):** dlopen runner pattern-copied from
  `tools/regress_bitexact.sh` + `clock_gettime(CLOCK_MONOTONIC)` timers.
  Times `plugin_open`, `plugin_get_header`, and a **serial ascending
  frame loop** — XDS's exact call pattern; that is the load-bearing
  design decision. Two modes: A/B (two `.so` paths, same fixture, ratio
  table) and multi-process (fork P runners over disjoint frame ranges,
  approximating `xds_par`'s J-jobs-each-serial structure). CSV/JSON-lines
  output: per-phase totals + per-frame min/p50/p95/max. The protocol doc
  is the canonical cloud run-book (192-core node, GeeseFS mount options,
  cache-drop procedure, 3600-frame dataset ID, 3 repetitions, acceptance
  thresholds) and the rule that every perf PR attaches a local A/B CSV.
- **NEGGIA-005 (cache):** cache parsed `Dataset`/data-file handles per
  worker; fix the 16× master-mmap `plugin_open` regression (construct
  one `H5File`, `shared_ptr`-copy per audit Inv-B); delete the dead
  file-scope `retVal` at `H5ToXds.cpp:422`; make `plugin_close` set
  `*error_flag`. Spec adopts **dataset-index dispatch**
  (`globalFrame / nframesPerDataset % K`) so consecutive frames hit the
  same warm worker (N_datasets total parses instead of K×N_datasets) —
  one line, bit-exact-neutral. The audit must formally **retire Inv-A
  (write-once-after-header)** in favor of "worker state is mutated only
  by its owning thread": lazy caching is a get_data-path mutation and
  must be blessed explicitly, not slipped in.
- **NEGGIA-006/007 (ring):** C++11 floor — mutex+condvar, no
  `shared_mutex`/`jthread`/`latch`. `frame_number` is 1-based
  (`correctFrameNumberOffset`, `H5ToXds.cpp:158-163`) while dispatch
  uses the raw value — a sharp edge for slot indexing the spec must pin.
- **NEGGIA-010 (pread) is evidence-gated, not speculative.** For a
  serial caller with cache+ring landed, pread changes syscall shape but
  not overlap; GeeseFS readahead may already coalesce. It carries the
  001 audit's Devil's-Advocate demand (network-FS coherence invisible
  to local bit-exact gates — require a network-FS verification run or an
  explicit local-SSD-only declaration with scientists-in-cloud as the
  gating signal). It proceeds only if NEGGIA-009 misses the ≤10 s I/O
  target, using 009's per-phase numbers to size the residual.

## Sequence dependencies (amended)

```
W0: 001 closeout + this amendment                     (human: merge PR #28)
W1: 002 ∥ 003 ∥ 004                                   (3 independent patches, one human gate)
W2: 005                                               (needs 002 for A/B, 003 for gates, 004 for meaningful stress)
    └─ optional interim cloud spot-run — first credible measured speedup lives HERE
W3: 006 → 007, 008 ∥                                  (one human gate applies all three)
W4: 009 cloud campaign → decision gate
W5: [010 if needed → re-run 009] → 011 → xds-side version-bump ticket
```

## Risk register (amended)

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| TSan/Helgrind clean but real GeeseFS-S3 race | Low | High | Manual scientists-in-cloud validation before release sign |
| **Speedup attribution impossible without harness** | was certain | High | NEGGIA-002 lands first; every perf PR attaches A/B CSV |
| **Eiger-16M ring memory budget (1 GB at 16 slots × 64 MB)** | Med | Med | NEGGIA-008 tunables land before any cloud run; protocol doc records slot sizing per detector |
| 50-line cap blows on FrameRing (006) | High (projected 55) | Low | Pre-negotiated trim-or-exemption at intake; HALT surfaces it cleanly |
| `pread` performance regression on local SSD (no FUSE) | Low | Med | 010 is conditional; spec invariant: local-SSD baseline ≥ current |
| ABI drift slips past audit | Very low | Critical | Hard-coded `nm -D` parity check in `neggia-verify`; sacred per Hard Rule 4 |
| Tier 1 ships but `xds_par` doesn't see speedup | Med → lowered by re-plan | Med (sad day) | Ring prefetch (007) creates in-plugin overlap independent of caller concurrency; harness multi-process mode models `xds_par` before the cloud run |

## Tier-2 outlook (out of scope here)

Unchanged: the RFC's Tier 2 (`plugin_hint_frame_range` +
`plugin_get_data_batch`) is additive ABI extension, only if Tier-1
falls short after NEGGIA-009/010 — new tickets of type `abi-evolution`
coordinating with matching xds-side tickets.

## Realistic timeline (amended)

- W1 (002 ∥ 003 ∥ 004): 1 session
- W2 (005 + interim spot-run): 1–2 sessions
- W3 (006 → 007, 008): 1–2 sessions
- W4 (009 campaign): 1 session + cloud turnaround
- W5 (010 if needed, 011 + xds bump): 1 session

Total: 5–7 focused sessions; first credible measured speedup after W2.

---

## Historical record — original stage descriptions (pre-amendment)

Preserved verbatim-in-substance for audit-trail purposes; superseded by
the amended table above.

- **Stage 1 — NEGGIA-001 — Worker pool replacing `GLOBAL_HANDLE`
  singleton.** Shipped (PR #28): 16 pre-parsed `H5DataCache` workers,
  `frame % 16` dispatch, `Test_XdsPluginConcurrent`, TSan clean locally,
  bit-exact on 9 fixtures, 67/50 cap exemption (reporter-authorized;
  sum-vs-net methodology mismatch since codified in `neggia-deep-audit`).
  Original claim of "~1.3–1.5× at this stage" withdrawn per Amendment
  record §1.
- **Stage 2 (old NEGGIA-002) — pread chunk-payload path** → now
  conditional NEGGIA-010.
- **Stage 3 (old NEGGIA-003) — pre-decoded frame ring buffer** → now
  NEGGIA-006/007.
- **Configuration (old NEGGIA-004) — env tunables** → now NEGGIA-008.
- **Release (old NEGGIA-005) — cut v1.3.0** → now NEGGIA-011; the
  downstream xds bump ticket named "XDS-042" in the original text is
  reassigned to the next free XDS ID at bump time.
