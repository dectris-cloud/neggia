# Learning: NEGGIA-001 — Worker pool replacing GLOBAL_HANDLE singleton

First real C++ ticket through the neggia framework (2026-05-29 patch;
2026-08-18 closeout). Four lessons, most consequential first.

## 1. Validate the caller's concurrency model before designing for concurrency

The ticket's premise — "surfaces the parallelism XDS already has above
the plugin" — was wrong, and the evidence was in the RFC that spawned
it: XDS calls `plugin_get_data` one frame at a time
(`xds/src/formlib/generic_getfrm.f90:99`). A thread-less sharded pool
cannot speed up a serial caller; the concurrency has to be created
*inside* the plugin (prefetch threads, NEGGIA-007). The framework's
spec/audit steps both accepted the premise because it lived in the
Priority field — prose no acceptance test exercised. **Lesson: any perf
ticket's spec must state who provides the concurrency and cite the call
site; "measurable prediction" beats "plausible narrative".** The
2026-08-18 tier-1 plan amendment reordered the whole program around
this (harness → cache → ring; pread demoted to evidence-gated).

## 2. Cap projections must use the same arithmetic as the cap gate

The audit projected 17–27 cap units (net delta); `neggia-surgical-patch`
counts the sum of removed + added with 1.5× header weight, and HALTed
at 67/50. Same diff, two arithmetics, one surprise HALT and a
reporter-direct exemption. Codified 2026-08-18: `neggia-deep-audit`'s
Notes now mandate sum-counting (xds repo, XDS-072 wave). **Lesson:
every projection an upstream step hands a downstream gate must state
its counting rule and match the gate's.**

## 3. Audited invariants are only load-bearing if the patch is diffed against them

Audit Inv-B reasoned that per-worker ownership was cheap because K
workers would share ONE master mmap via `shared_ptr` copy. The shipped
patch constructs `H5File(filename)` independently in the K-loop
(`H5ToXds.cpp:437-443`) — 16 sequential open+mmap of the master per
`plugin_open`, silently restated in the patch proposal as intentional.
Nobody diffed patch-against-invariants at the apply gate. Assigned to
NEGGIA-005 (one shared handle, `shared_ptr`-copied). **Lesson: step 5
verification should include an explicit "patch honors each audited
invariant" checklist, not only build/test gates.** Same gap let three
audit-flagged anomalies (dead `retVal` at `:422`, `plugin_close` never
setting `*error_flag`, per-frame Dataset re-parse) go unfiled for
eleven weeks — now folded into NEGGIA-005.

## 4. A verification matrix is only as real as its infrastructure

AC-5 (Helgrind) was unrunnable from day one: no valgrind on macOS
arm64, no valgrind lane in CI. AC-3 specified `datasets_eiger1/` but
the implemented test uses the 5-frame synthetic fixture, so workers
6–15 have never executed under any tool. Both were discovered at
closeout, not at spec review. **Lesson: every AC needs an execution
venue named at spec time (local / which CI lane / manual protocol);
"deferred to Linux CI" requires the lane to exist.** NEGGIA-003
(TSan + Helgrind lanes) and NEGGIA-004 (fixture upgrade) are now hard
prerequisites for the threaded tickets, and retro-produce NEGGIA-001's
missing evidence on merged master.

## Positive patterns worth repeating

- `tools/regress_bitexact.sh` (dlopen A/B runner, fixture
  auto-discovery, `cmp` per frame) proved the right shape — NEGGIA-002's
  benchmark harness pattern-copies it.
- The HALT-then-explicit-exemption flow worked as designed: the cap
  overrun was surfaced, priced (~1.3×), rationalized (no-op intermediate
  commit avoided), precedent-cited (XDS-036 at ~3.1×), and recorded in
  the ticket. Governance friction ≠ governance failure.
- Inv-A (write-once-after-`plugin_get_header`) was a genuinely useful
  discovery — it made the thread-confinement design cheap. NEGGIA-005's
  audit must now formally retire it (lazy caching mutates on the
  get_data path) rather than let it silently rot.
