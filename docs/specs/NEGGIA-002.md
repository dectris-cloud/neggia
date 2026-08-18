# Spec: NEGGIA-002 — Benchmark harness + cloud measurement protocol

## Goal
Create the measurement capability every Tier-1 perf claim depends on: a dlopen-based benchmark runner (`tools/bench_frames.sh`) that times the plugin exactly the way XDS calls it (serial ascending frames), plus the canonical cloud run-book (`docs/benchmarks/protocol.md`). After landing, every perf ticket's PR attaches a local A/B CSV against the frozen PR-#28-merge baseline `.so`, and NEGGIA-009's cloud campaign runs a written, repeatable protocol. No production code changes.

## Affected Source
- `tools/bench_frames.sh` (NEW) — shell driver + embedded C++11 runner, pattern-copied from `tools/regress_bitexact.sh`
- `docs/benchmarks/protocol.md` (NEW) — cloud run-book + evidence conventions
- `CHANGELOG.md` — `[Unreleased]` → Added

## Invariants
1. **ABI untouched:** `nm -D <built .so> | grep -cE '\b(plugin_open|plugin_close|plugin_get_header|plugin_get_data)\b'` = 4, matching `docs/abi-baseline.txt`; the harness consumes the symbols via `dlopen`/`dlsym` and modifies nothing — source of truth: `docs/abi-baseline.txt`
2. **Zero production diff:** the ticket's PR touches only `tools/`, `docs/`, `CHANGELOG.md` — `git diff --name-only master... | grep -c '^src/'` = 0
3. **Call-pattern fidelity:** the default frame loop is serial, ascending, one `plugin_get_data` per frame — the same contract as XDS's caller (`xds/src/formlib/generic_getfrm.f90:99`); any other order is opt-in via flag, never default
4. **Timer discipline:** all timings from `clock_gettime(CLOCK_MONOTONIC)`; recorded in ns internally, reported in ms with ≥3 significant digits; phases timed separately: `plugin_open`, `plugin_get_header`, frame loop (total + per-frame min/p50/p95/max)
5. **A/B stability floor:** self-test with A = B (same `.so` twice, same fixture, 3 repetitions) reports per-phase ratios within [0.8, 1.25] — source of truth: ticket AC; this bounds harness noise so a claimed 1.3× stage gain is distinguishable from measurement error
6. **Exit-code contract:** 0 = success; 2 = fixture yielded zero readable frames; any dlopen/dlsym/plugin-error path = nonzero ≠ 2
7. **Cap accounting:** infrastructure script ~110 lines — Exception C requested at intake (whole-file review, `regress_bitexact.sh` precedent); fallback pre-scoped 2-way split. Human ruling at step 4 — source of truth: ticket Constraints

## Acceptance Tests
1. **Local run, both eiger fixtures** — type: benchmark
   - Setup: `.so` built from master; `h5-testfiles` submodule present
   - Action: `tools/bench_frames.sh <so> src/dectris/neggia/test/h5-testfiles/datasets_eiger1/<master>.h5` (and eiger2)
   - Expected: exit 0; CSV with the four phase rows; per-frame stats present; frame count matches the fixture's known total
2. **A=B stability self-test** — type: benchmark
   - Action: `tools/bench_frames.sh --ab <so> <so> <master.h5>` ×3
   - Expected: exit 0; all per-phase ratios in [0.8, 1.25]
3. **Multi-process mode** — type: benchmark
   - Action: `tools/bench_frames.sh --procs 4 <so> <master.h5>`
   - Expected: exit 0; 4 per-process rows over disjoint frame ranges + aggregate wall row
4. **Empty-fixture guard** — type: benchmark
   - Action: run against a path with no readable frames
   - Expected: exit 2
5. **Protocol doc completeness** — type: ctest (review gate)
   - Expected: `docs/benchmarks/protocol.md` contains: node spec, GeeseFS mount options, cache-drop procedure, canonical dataset ID, 3-repetition rule, acceptance thresholds (I/O ≤ 10 s, wall ≤ 60 s), baseline-`.so` sha256 recording rule, per-PR A/B CSV convention

## Out of Scope
- Any production source change (`src/dectris/neggia/**`)
- Wiring the harness into CI (a later ticket may add a perf-smoke lane; not here)
- Randomized/strided frame-order modes (flag reserved, not implemented)
- The cloud campaign itself (NEGGIA-009) and any GeeseFS tuning
- Multi-threaded single-process mode (would mis-model the serial caller)

## Open Questions
1. Which `plugin_get_header` output slot carries the total frame count (info array index?) — audit quotes the contract from `H5ToXds.cpp`/`regress_bitexact.sh`
2. Exact compile line + fixture-discovery idiom to pattern-copy from `regress_bitexact.sh` — audit quotes it
3. Per-frame timing overhead: is one `clock_gettime` pair per frame (~40-60 ns) negligible vs the ~ms-scale frame reads on all fixtures? (Expected yes; audit sanity-checks the smallest fixture)
