# Ticket: NEGGIA-002 — Benchmark harness + cloud measurement protocol

## Status
Open

## Type
infra

## Priority
P1 — Wave-1 enabler of the amended Tier-1 plan (`docs/plans/tier-1-performance.md` §Stages, amended 2026-08-18). Lands FIRST: no speedup claim in the plan is currently measurable, and the user's top priority is "speedup done so benchmarking can start".

## Origin
- Reporter: Max Burian (max.burian@dectris.com)
- Date: 2026-08-18
- Source: NEGGIA-001 closeout finding 3 (see `docs/learnings/NEGGIA-001.md` and the tier-1 plan §Amendment record): no timing code exists anywhere in the repo — only correctness tooling (`tools/regress_bitexact.sh`). Every per-stage multiplier in the pre-amendment plan was unfalsifiable.

## Problem Statement
The Tier-1 program promises quantified speedups (90 s → ≤60 s wall, 40 s → ≤10 s I/O on the canonical cloud benchmark) but the repo contains no way to measure plugin performance: no benchmark binary, no timing harness, no protocol for the scientists-in-cloud validation run, and no convention for attaching evidence to perf PRs. Perf tickets NEGGIA-005/007/010 cannot pass honest gates without one, and the pread stage (NEGGIA-010) is explicitly *evidence-gated* on numbers this harness must produce.

## Expected Behavior
1. `tools/bench_frames.sh` — a dlopen-based benchmark runner pattern-copied from `tools/regress_bitexact.sh` (heredoc C++ runner, `$CXX -std=c++11 -O2 ... -ldl`), adding `clock_gettime(CLOCK_MONOTONIC)` timers. It times, separately: `plugin_open`, `plugin_get_header`, and a **serial ascending frame loop** (XDS's exact call pattern — the load-bearing design decision). Output: CSV/JSON-lines — per-phase totals plus per-frame latency min/p50/p95/max.
2. Two modes: **A/B** (two `.so` paths, same fixture, prints a ratio table) and **multi-process** (fork P runners over disjoint frame ranges, approximating `xds_par`'s J-jobs-each-serial structure). No multi-threaded single-process mode — that would mis-model the caller.
3. `docs/benchmarks/protocol.md` — canonical cloud run-book: 192-core node spec, GeeseFS mount options, page-cache drop procedure, canonical 3600-frame Eiger dataset ID, 3 repetitions, acceptance thresholds (I/O ≤ 10 s, wall ≤ 60 s), and the standing rule: **every perf ticket's PR attaches a local A/B CSV** (post-patch vs the frozen PR-#28-merge baseline `.so`).

## Acceptance Criteria
- [ ] `tools/bench_frames.sh <path.so> <master.h5>` runs against `datasets_eiger1` and `datasets_eiger2` fixtures locally and emits well-formed CSV with the four phases (open / header / frame-loop total / per-frame stats)
- [ ] A/B mode: `tools/bench_frames.sh --ab <A.so> <B.so> <master.h5>` prints per-phase ratios; self-test A=B on the same `.so` reports ratios within 0.8–1.25 across 3 repetitions (stability check)
- [ ] Multi-process mode: `--procs P` forks P runners over disjoint frame ranges and reports per-process + aggregate wall time
- [ ] Exit codes: 0 on success, 2 if the fixture has no readable frames, nonzero on dlopen/symbol failure
- [ ] `docs/benchmarks/protocol.md` exists with the run-book content in Expected Behavior item 3
- [ ] No production source touched: diff confined to `tools/`, `docs/`, `CHANGELOG.md`
- [ ] All existing CI lanes stay green (harness is not wired into CI in this ticket)

## Constraints
- **Touches:** `tools/bench_frames.sh` (NEW), `docs/benchmarks/protocol.md` (NEW), `CHANGELOG.md`
- **ABI impact:** NO (consumes the 4 symbols via dlopen; changes nothing)
- **Concurrency surface:** NO threading in production code; the multi-process mode uses `fork` in the harness only
- **Coordinated XDS-fork update:** no
- **Cap:** ~110 lines of infrastructure script — **Exception C request declared at intake** (measurement/verification tooling reviewed whole-file, mirroring xds Hard Rule 3 Exception A's review-safety argument: validate the runner once, not line-by-line; precedent `tools/regress_bitexact.sh`, which landed under NEGGIA-001's exemption). **Fallback if the human declines:** pre-scoped 2-way split — (a) C++ runner, (b) shell driver + protocol doc.
- **Reproducer:** n/a (the ticket creates the measurement capability)

## 8-Step Workflow Log
1. **Ticket:** this file (created 2026-08-18)
2. **Requirement Spec:** `docs/specs/NEGGIA-002.md` (to be written — Wave 2)
3. **Audit & Challenge:** `docs/audits/NEGGIA-002.md` (light: no production code; audit focuses on timer correctness + fixture discovery)
4. **Minimal Patch Proposal:** Exception C ruling needed from the human at this gate
5. **Apply & Verify:** local run on eiger fixtures; A=B stability self-test
6. **Commit/PR:** branch `feature/NEGGIA-002-benchmark-harness` → master (after PR #28 + closeout PR merge)
7. **Changelog:** `[Unreleased]` → Added
8. **Learning:** optional

## Notes
- Frozen A/B baseline: the `.so` built from PR #28's merge commit (record its sha256 in the protocol doc at first use).
- The harness must keep the frame loop **serial ascending** by default; any future randomized-order mode must be a flag, never the default (XDS never calls out of order).
- Timer resolution note for the spec: per-frame latencies on local SSD can be tens of µs; use `CLOCK_MONOTONIC` (not `CLOCK_REALTIME`) and report ns internally, ms in the CSV.
- Depends on: nothing (first Wave-1 ticket). NEGGIA-005/007/010 depend on this.
