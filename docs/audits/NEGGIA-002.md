# Audit: NEGGIA-002 — Benchmark harness + cloud measurement protocol

## Spec Reference
../neggia/docs/specs/NEGGIA-002.md

## Symbol / File Inventory
(from neggia-archaeologist, 2026-08-18)

- Pattern source `tools/regress_bitexact.sh` (141 lines): bash driver (`set -euo pipefail`) → heredoc C++ runner → compile `"$CXX" -std=c++11 -O2 -o "$RUNNER_BIN" "$RUNNER_SRC" -ldl` (lines 92-93, `CXX="${CXX:-c++}"`) → per-fixture runs. Fixture discovery: `find "$fixture_dir" -type f -name '*master*.h5'` (line 127). Exit codes: driver 2 = usage/missing-so/zero fixtures, 1 = mismatch, 0 = pass; runner 2 = dlopen/dlsym, 3 = open/header/OOM, 4 = get_data error.
- Plugin contract (`plugin/H5ToXds.h:19-26`, `H5ToXds.cpp:490-495`): **total frame count is `plugin_get_header`'s 6th out-parameter `int* number_of_frames` = nimages × ntrigger — NOT an info-array slot** (spec OQ1 resolved). `info[1024]` carries only vendor/version (`setInfoArray`, `H5ToXds.cpp:412-418`). `plugin_get_data` fills nx·ny int32; buffer is caller-allocated from header dims. Error-flag convention: pre-set `err=1` before calls so a silent plugin fails loudly (`regress_bitexact.sh:63-64`).
- Runner's serial ascending loop (`regress_bitexact.sh:74-79`): `for (int f = 1; f <= total; ++f) data_f(&f, ...)` — 1-based, exactly XDS's call pattern; this is the loop the harness times.
- Fixture inventory (frame counts verified from test expected-values): eiger1 = 4 masters, 1030×1065 uint16, 4 frames each (2-datafile variants: 2 frames/file); eiger2 = 5 masters, 1028×512, uint8/16/32, 4 frames; `dataset_artificial_small_001` = 11×13, 5 frames; `dataset_artificial_large_001` = 11×13, **5000 frames / 1000 data files**. `datasets_different_h5ver/` has no `*master*.h5` → invisible to the discovery idiom (note for protocol doc).

## Threading Model
No production threading touched. The harness's `--procs P` mode uses `fork()` in the *driver/runner only* — P independent processes over disjoint frame ranges, modeling `xds_par`'s J-jobs-each-serial structure. No shared memory between processes; no synchronization primitives introduced anywhere.

## Call Graph
Harness (new leaf consumer) → dlopen/dlsym → the four `plugin_*` symbols → existing internals. No production symbol gains a caller-visible change; the harness is a sibling of `regress_bitexact.sh` in the consumer graph.

## ABI Surface Impact
**NONE.** Tools + docs only. `nm -D` parity trivially preserved (spec invariant 1). ABI HALT does not trigger. XDS-fork Fortran caller (`generic_data_plugin.f90:140-170`) re-verified this session — still matches `H5ToXds.h:17-35`.

## Invariants
1. Serial-ascending default loop, 1-based frames (spec inv 3) — template at `regress_bitexact.sh:74-79`
2. **Channel separation**: `regress_bitexact.sh`'s runner writes binary to stdout, diagnostics to stderr. The bench runner must NOT copy the binary-dump; it emits CSV to stdout, diagnostics to stderr — mixing channels is the one correctness hazard in pattern-copying (archaeologist risk note)
3. `CLOCK_MONOTONIC`, ns internal / ms reported (spec inv 4); timer overhead ≤1-3% even on the 11×13 synthetic worst case, <0.01% on eiger (spec OQ3 resolved)
4. Exit-code contract (spec inv 6) maps onto the existing convention; "no readable frames" → 2 aligns with driver's zero-fixtures exit 2
5. A/B stability [0.8, 1.25] over 3 reps (spec inv 5)
6. Error-flag discipline: pre-set nonzero before every plugin call (pattern line 63-64)

## Risks
- **Low** overall (no production code). Specific: (i) channel-mixing (invariant 2); (ii) honesty risk — local eiger fixtures are 4 frames: a local A/B measures open/header + per-frame decode, NOT sustained I/O; the protocol doc must state this explicitly so nobody reads a local CSV as a cloud claim; (iii) Exception C ruling is a process gate at step 4, not a technical risk (fallback 2-way split pre-scoped).

## Existing ctest Coverage on Surface
None applies (no production change). `regress_bitexact.sh` remains the correctness harness; bench_frames.sh is perf-only. No CI wiring in this ticket (spec out-of-scope).

## Challenge Questions Answered
- Frame-count source? 6th out-param of `plugin_get_header` (OQ1) — quoted above.
- Compile line + discovery to copy? `regress_bitexact.sh:92-93` and `:127` (OQ2).
- Timer overhead? Negligible on all fixtures; vanishes on eiger/cloud data (OQ3).
- Who else calls the four symbols? In-tree tests (3) + regress_bitexact + XDS-fork; the harness adds a read-only consumer.

## Devil's Advocate
- **Strongest argument against:** the harness's local numbers are structurally incapable of validating the thing that matters (GeeseFS sustained I/O on 3600 frames), so attaching a local A/B CSV to every perf PR could create *false confidence* — a cache-warm 4-frame decode ratio says nothing about FUSE readahead behavior, and reviewers may treat green ratios as cloud evidence.
- **Resolution:** the protocol doc gets a mandatory "what local A/B does and does not measure" section (invariant/risk ii), and the plan already separates local evidence (attribution + regression guard) from NEGGIA-009 (the only source of cloud claims). Local A/B's real job is *relative attribution* — did this patch change open cost, header cost, per-frame decode — which 4-frame fixtures measure fine.
- **Second-order effects:** none on numerical accuracy or the Fortran caller (read-only consumer). Worst case is wasted reviewer attention on noisy CSVs — bounded by the [0.8,1.25] stability self-test.
- **What would make this audit wrong:** if `plugin_open`+`get_header` cost dominated so heavily on 4-frame fixtures that per-frame timing drowned in noise — mitigated by phase-separated timers (that dominance would itself be a visible, correct result).

## Cap-Unit Projection (sum-counted)
`tools/bench_frames.sh` ~110 lines (infrastructure script — cap-bound per CLAUDE.md Hard Rule 3; **Exception C requested at intake**, fallback 2-way split); `docs/benchmarks/protocol.md` cap-exempt (docs). No header files.

## Audit Verdict
- **READY_FOR_PATCH**
- Rationale: scope matches the spec exactly (2 new files, zero production contact); all 3 OQs resolved with quoted sources; the only substantive risk (local-vs-cloud evidence confusion) is closed by a mandatory protocol-doc section; the Exception C process gate is declared and pre-scoped with a fallback. Devil's Advocate finds no structural objection.

## Minimal Patch Proposal

Source-line count: **99 counted / 50 cap — Exception C ruling required** (121 raw lines in `tools/bench_frames.sh`, 99 non-comment/non-blank; `docs/benchmarks/protocol.md` 85 lines, docs-exempt; zero lines under `src/dectris/neggia/`; no headers). Fallback if Exception C is declined: pre-scoped 2-way split (a: C++ runner file, b: shell driver + protocol doc).

### Implementation (authored on `feature/NEGGIA-002-benchmark-harness`; whole-file review)

- `tools/bench_frames.sh` — dlopen runner (heredoc, `-std=c++11 -O2 -ldl`, pattern: `regress_bitexact.sh:92-93`); phases timed separately with `CLOCK_MONOTONIC`; serial ascending 1-based frame loop; modes: single / `--ab` (discarded warmup + 3 reps each, ratio table on stderr) / `--procs` (forked disjoint ranges + aggregate wall). **Sentinel-prefixed output (`BENCH:`)**: verification surfaced that the plugin banners on *stdout* during `plugin_open` — unfiltered, it corrupts any machine-readable stdout (it also rides inside `regress_bitexact.sh`'s binary dumps, harmlessly only because it is symmetric).
- `docs/benchmarks/protocol.md` — per-PR A/B convention, frozen baseline (`1b016d0` merge build), cloud campaign run-book with cache discipline + acceptance thresholds + NEGGIA-010 decision rule, and the mandatory "what local A/B does NOT measure" section (Devil's-Advocate resolution).

### Per-hunk justification
1. `tools/bench_frames.sh` (new) — spec invariants 3 (call-pattern fidelity), 4 (timer discipline), 5 (A/B stability), 6 (exit codes); ATs 1-4
2. `docs/benchmarks/protocol.md` (new) — spec AT-5 (protocol completeness); audit risk (ii) closure
3. No other files; CHANGELOG deferred to post-merge sweep (parallel-branch conflict avoidance; step 7 pending)

### Verification (executed locally 2026-08-18, master `.so` build `1b016d0`+PR29)
- AT-1: eiger1 + eiger2 bslz4 runs — exit 0, well-formed CSV, frames=4 matches fixtures ✓
- AT-2: A=B self-test ×3 invocations — ratios open/header/loop all within [0.8, 1.25] (observed 0.84–1.04) ✓
- AT-3: `--procs 2` — 2 rows, disjoint ranges (2+2 frames), aggregate wall reported, exit 0 ✓
- AT-4: missing fixture → exit 2 ✓
- ABI: no source touched; `nm -D` parity trivially preserved ✓
- TSan/Helgrind/bit-exact: N/A (no production code; skill rule 5 gate applies only to threading-state changes)

### Verdict
- **READY_FOR_HUMAN_APPLY** (pending the Exception C ruling at the PR — merge = accept; request split = fallback plan activates)
