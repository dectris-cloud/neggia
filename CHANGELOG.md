# Changelog

All notable changes to `dectris-cloud/neggia` are documented here. Format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed
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
