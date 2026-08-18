# Neggia benchmark protocol

> Authored by NEGGIA-002 (2026-08-18). Governs (a) the per-PR local A/B
> evidence convention for every perf ticket, and (b) the canonical
> scientists-in-cloud campaign (NEGGIA-009) that produces release-gating
> numbers. Harness: `tools/bench_frames.sh`.

## What local A/B does — and does NOT — measure

The in-repo fixtures are **4-frame** Eiger sets (eiger1: 1030×1065 uint16;
eiger2: 1028×512 uint8/16/32; bslz4/lz4; 2-datafile variants carry 2
frames/file). A local A/B run therefore measures **relative attribution**:
did the patch change `plugin_open` cost, `plugin_get_header` cost, or
per-frame decode+read cost — on a warm page cache, at millisecond scale.

It does **not** measure sustained I/O, FUSE/GeeseFS readahead behavior, or
anything about the 3600-frame cloud workload. **Never quote a local CSV as
a cloud claim.** Cloud numbers come only from the campaign below.

Known small-sample artifacts (harmless, do not "fix" them): with ≤4 frames
the p95 column can read below p50 (index math on tiny n); in `--ab` mode a
systematic ratio lean of ~0.85–0.95 favoring the second-run binary remains
after the warmup discard (page-cache residue at ms scale). The acceptance
band [0.8, 1.25] accounts for both.

## Per-PR evidence convention (every perf ticket)

1. Build the candidate `.so` from the PR branch.
2. Baseline `.so` = the frozen **PR-#28 merge** build (master `1b016d0`);
   record its sha256 next to the CSV the first time you use it.
3. Run, from the repo root:
   ```
   tools/bench_frames.sh --ab <baseline.so> <candidate.so> \
     src/dectris/neggia/test/h5-testfiles/datasets_eiger1/eiger1_testmode10_2datafiles_4images_bslz4_master.h5
   tools/bench_frames.sh --ab <baseline.so> <candidate.so> \
     src/dectris/neggia/test/h5-testfiles/datasets_eiger2/eiger2_simread7_2datafiles_4images_bslz4_uint32_master.h5
   ```
4. Attach both CSVs + the two `ratio(B/A):` stderr lines to the PR
   description. State one sentence of interpretation (which phase moved,
   why that matches the patch's mechanism).
5. Sanity gate: an A=A self-test (`--ab` with the same `.so` twice) must
   stay within [0.8, 1.25] on your machine before you trust any A/B.

## Canonical cloud campaign (NEGGIA-009; release-gating)

- **Node:** 192-core Dectris-Cloud node (same class as the XDS-037
  baseline measurement).
- **Storage:** GeeseFS-mounted S3 bucket. Record: GeeseFS version, mount
  options (`--memory-limit`, `--read-ahead-large`, cache dirs), bucket
  region. Mount options MUST be identical across baseline and candidate.
- **Dataset:** the canonical 3600-frame Eiger set from XDS-037 (2 data
  files; record the dataset ID/URI in the results doc).
- **Cache discipline:** unmount + remount GeeseFS between repetitions;
  `echo 3 > /proc/sys/vm/drop_caches` before each run.
- **Repetitions:** 3 per binary, alternating baseline/candidate (A B A B
  A B) to cancel drift.
- **Harness modes:** one serial pass (`bench_frames.sh SO MASTER`) for
  per-phase attribution, plus `--procs P` with P = the `xds_par`
  JOB-count used in production (models J-jobs-each-serial). Additionally
  run the real `xds_par` job (`JOB= XYCORR INIT COLSPOT IDXREF DEFPIX
  INTEGRATE CORRECT`) and record wall-clock + the I/O share.
- **Acceptance (v1.3.0 gate, from the tier-1 plan):** visible I/O ≤ 10 s,
  `xds_par` wall-clock ≤ 60 s (baseline: 40 s I/O / 90 s wall).
- **Record:** all CSVs + xds_par timings + environment block into
  `docs/learnings/NEGGIA-009-benchmark.md`, summary into `CHANGELOG.md`.
- **Decision rule:** if I/O > 10 s, NEGGIA-010 (pread) activates, sized by
  the per-phase residual this campaign reports.

## Harness reference

```
tools/bench_frames.sh SO MASTER.h5              # serial, full frame range
tools/bench_frames.sh --ab SO_A SO_B MASTER.h5  # 3 reps each + ratio table (stderr)
tools/bench_frames.sh --procs P SO MASTER.h5    # P forked runners, disjoint ranges
```

CSV columns: `so,fixture,proc,rep,open_ms,header_ms,frames,loop_ms,min_ms,p50_ms,p95_ms,max_ms`.
Timers: `CLOCK_MONOTONIC`, ns-resolution internally, ms in the CSV. The
frame loop is **serial ascending, 1-based** — XDS's exact call pattern
(`generic_getfrm.f90:99`); no other order exists in the harness by design.
Exit codes: 0 ok; 2 usage/missing file/no frames; 3/4 plugin errors
(open/get_data); the runner pre-sets `error_flag=1` before every call so a
plugin that never writes it fails loudly. Note: the plugin banners on
stdout during `plugin_open`; harness output is sentinel-prefixed and
filtered, so CSVs stay clean.
