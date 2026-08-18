# Ticket: NEGGIA-003 — TSan + Helgrind CI lanes

## Status
Open

## Type
infra

## Priority
P1 — Wave-1 enabler (amended tier-1 plan §Stages). Hard prerequisite for the threaded tickets NEGGIA-006/007: the governance gates (`neggia-verify` §concurrency matrix) mandate TSan + Helgrind per threading ticket, the dev machine (macOS arm64) cannot run valgrind at all, and no CI lane runs either tool today.

## Origin
- Reporter: Max Burian (max.burian@dectris.com)
- Date: 2026-08-18
- Source: NEGGIA-001 closeout finding 4 (`docs/learnings/NEGGIA-001.md`): AC-5 (Helgrind) was unrunnable from day one — deferred "to Linux CI" while `.github/workflows/main.yml` contains no valgrind step and no TSan lane. The only concurrency-tool evidence NEGGIA-001 ever produced is one local TSan run on a 5-frame synthetic fixture.

## Problem Statement
`.github/workflows/main.yml` builds and ctests on 6 platform/compiler combos but never runs ThreadSanitizer or Helgrind. Consequences: (a) NEGGIA-001's AC-5 remains evidence-less on merged code; (b) NEGGIA-006/007 (ring + prefetch threads — the target-reaching tickets) cannot pass their own mandatory gates; (c) any latent race in the worker-pool code ships unwatched. Deferring concurrency tooling to per-ticket local runs is structurally broken on this project because the maintainer's machine is macOS arm64 (no valgrind).

## Expected Behavior
Two new ubuntu-24.04 jobs in `.github/workflows/main.yml`:

1. **TSan lane:** Debug build with `-fsanitize=thread -g` (CMake flags, mirroring NEGGIA-001's local `build-tsan` recipe), running the full ctest suite. Any `WARNING: ThreadSanitizer` → job failure.
2. **Helgrind lane:** normal Debug build; `valgrind --tool=helgrind --error-exitcode=1` on the concurrency-relevant tests only (`Test_XdsPluginConcurrent`, `Test_XdsPlugin`) — full-suite Helgrind is too slow for per-PR CI.

Both jobs run on every PR + push, alongside (not replacing) the existing matrix. First green run on merged master retroactively produces the Helgrind evidence NEGGIA-001's AC-5 deferred.

## Acceptance Criteria
- [ ] TSan job: `ctest --output-on-failure` green under `-fsanitize=thread`; grep of job log for `WARNING: ThreadSanitizer` is empty; a seeded-race canary check is NOT required (out of scope) but the job must demonstrably fail on nonzero ctest exit
- [ ] Helgrind job: `valgrind --tool=helgrind --error-exitcode=1` on `Test_XdsPluginConcurrent` + `Test_XdsPlugin` exits 0; job wall-time ≤ 15 min
- [ ] Both jobs appear in the PR checks list and are required-passing for this and subsequent PRs (branch-protection update is a human console action — record it in step 5)
- [ ] Existing 6-combo matrix untouched and green
- [ ] Workflow diff ≤ 50 cap units (projection ~40; CI workflows are cap-bound — xds XDS-069 precedent)
- [ ] NEGGIA-001 AC-5 evidence link recorded: first green Helgrind run URL on merged master appended to `tickets/closed/NEGGIA-001-*.md` Notes §Closeout

## Constraints
- **Touches:** `.github/workflows/main.yml`, possibly a ≤5-line CMake option plumb if the TSan flags need a toggle, `CHANGELOG.md`
- **ABI impact:** NO
- **Concurrency surface:** NO new threading; this ticket only *observes* it
- **Coordinated XDS-fork update:** no
- **Reproducer:** n/a (CI infrastructure)

## 8-Step Workflow Log
1. **Ticket:** this file (created 2026-08-18)
2. **Requirement Spec:** `docs/specs/NEGGIA-003.md` (Wave 2)
3. **Audit & Challenge:** `docs/audits/NEGGIA-003.md` (light: workflow-only; audit walks the vendored-gtest TSan interaction + valgrind availability on ubuntu-24.04 runners)
4. **Minimal Patch Proposal:** ≤50 cap units or HALT
5. **Apply & Verify:** lanes green on the PR itself (self-verifying)
6. **Commit/PR:** branch `feature/NEGGIA-003-tsan-helgrind-lanes` → master
7. **Changelog:** `[Unreleased]` → Added
8. **Learning:** optional

## Notes
- Vendored gtest under TSan may need the same `-Wno-error=maybe-uninitialized`-style accommodation the gcc lanes needed (NEGGIA-001 commit `2293e9e`) — spec should pre-check.
- Helgrind on dlopen'd `.so`s works but symbolization needs `-g` on the plugin build — the job should build Debug.
- Independent of NEGGIA-002/004 (different files); all three are Wave-1 parallel.
