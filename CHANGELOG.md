# Changelog

All notable changes to `dectris-cloud/neggia` are documented here. Format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed
- **Tier-1 plan amended + NEGGIA-001 closed** (2026-08-18): stage order
  re-planned to benchmark-harness → CI concurrency lanes + fixture
  upgrade → per-worker dataset cache → ring prefetch → tunables → cloud
  campaign, with pread demoted to an evidence-gated conditional
  (`docs/plans/tier-1-performance.md` §Amendment record documents the
  three driving findings: serial XDS caller, per-frame data-file
  re-parse cost at `user/Dataset.cpp:36,45`, and the absence of any
  benchmark harness). NEGGIA-001 moved to `tickets/closed/` with AC-3
  partial (fixture → NEGGIA-004) and AC-5 deferred (Helgrind →
  NEGGIA-003 lane) recorded; learning doc at
  `docs/learnings/NEGGIA-001.md`; the 16×-master-mmap Inv-B divergence
  and the three unfiled audit anomalies assigned to NEGGIA-005.
  Wave-1 tickets NEGGIA-002/003/004 opened. — closes NEGGIA-001
- **Worker pool replaces `GLOBAL_HANDLE` singleton** (NEGGIA-001 —
  Stage 1 of Tier-1 perf overhaul per XDS-037 RFC). The plugin layer
  now holds `NUM_WORKERS=16` `H5DataCache` instances, each with its
  own `H5File` (own mmap = own kernel readahead state — load-bearing
  for the GeeseFS-S3 concurrency win). `plugin_get_data` dispatches by
  `frame_number % NUM_WORKERS` for a lock-free hot path; per-worker
  state is thread-confined (per audit Inv-A: H5DataCache is
  write-once-then-immutable after `plugin_get_header`). External
  single-open contract preserved (stderr message verbatim on second
  open while pool active). The 4 sacred C ABI symbols (`plugin_open`,
  `plugin_close`, `plugin_get_header`, `plugin_get_data`) retain
  byte-identical signatures — XDS-fork's `tools/neggia-version.txt`
  bump path stays clean. New `Test_XdsPluginConcurrent` (16 threads ×
  100 calls, bit-equality vs single-threaded reference) verifies the
  concurrency surface; passes ctest + TSan clean (Helgrind verification
  deferred to Linux CI — unavailable on macOS arm64). Bit-exact
  regression against pre-patch `.so` verified on 9 fixtures across
  `datasets_eiger1` + `datasets_eiger2` (uint16/uint32/uint8;
  BSLZ4/LZ4/uncompressed; 0/2 data-file variants). Scientists-in-cloud
  GeeseFS-S3 benchmark target (≥1.3× wall-clock speedup) deferred to
  NEGGIA-005 release-time validation. Reporter-authorized cap exemption
  recorded (67/50 cap units ≈ 1.3× cap; precedent XDS-036; cap reaffirmed
  for subsequent tickets). — closes NEGGIA-001

## [1.2.0] — 2021-03-26

- Baseline upstream Kabsch / Dectris release (commit `a81bdf8a994f18c61a313f57810651a76a218f17`).
  Pre-fork; recorded here for completeness. Code unchanged from this
  commit; all subsequent versions land via NEGGIA-NNN tickets under the
  agent framework documented in `docs/architecture/agent-framework.md`.
