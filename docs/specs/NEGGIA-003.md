# Spec: NEGGIA-003 — TSan + Helgrind CI lanes

## Goal
Make the framework's mandatory concurrency gates executable: add a ThreadSanitizer lane and a Helgrind lane to `.github/workflows/main.yml` (ubuntu-24.04), running on every PR and push. The dev machine (macOS arm64) cannot run valgrind, so without these lanes the threaded tickets NEGGIA-006/007 cannot pass their own verification matrix — and NEGGIA-001's deferred AC-5 finally gets evidence on merged master.

## Affected Source
- `.github/workflows/main.yml` — two new jobs alongside the existing matrix
- (contingent, ≤5 lines) top-level `CMakeLists.txt` or cmake flags plumb IF the TSan flags cannot be passed purely via `-DCMAKE_CXX_FLAGS` — audit decides; default is no CMake change
- `CHANGELOG.md` — `[Unreleased]` → Added

## Invariants
1. **ABI untouched / zero source diff:** no file under `src/dectris/neggia/` changes; `nm -D` 4-symbol parity trivially preserved — source of truth: `docs/abi-baseline.txt`
2. **Existing matrix untouched:** the `posix-cc` and `posix-gcc` jobs' YAML is byte-identical pre/post except (at most) nothing — new jobs are purely additive
3. **TSan lane contract:** Debug build with `-fsanitize=thread -g` (NEGGIA-001's local recipe); full `ctest --output-on-failure`; job fails on nonzero ctest exit; `grep -c 'WARNING: ThreadSanitizer'` over the job log = 0
4. **Helgrind lane contract:** `valgrind --tool=helgrind --error-exitcode=1` on the concurrency-relevant test binaries only (`Test_XdsPluginConcurrent`, `Test_XdsPlugin`); exit 0 required; plugin + tests built Debug (`-g`) for symbolization
5. **Wall-time budget:** each new job ≤ 15 min on a standard ubuntu-24.04 runner
6. **Cap:** workflow diff ≤ 50 cap units (CI workflows are cap-bound; xds XDS-069 precedent) — projection ~40
7. **Evidence closure:** first green Helgrind run URL on merged master is appended to `tickets/closed/NEGGIA-001-*.md` Notes §Closeout (closes the AC-5 deferral)

## Acceptance Tests
1. **TSan lane green** — type: TSan
   - Setup: this ticket's PR
   - Action: CI runs the new TSan job
   - Expected: ctest exit 0; zero `WARNING: ThreadSanitizer` lines in the log
2. **Helgrind lane green** — type: Helgrind
   - Action: CI runs the new Helgrind job
   - Expected: valgrind exit 0 on both listed tests; job wall-time ≤ 15 min
3. **Failure propagation** — type: ctest
   - Action: rely on `--error-exitcode=1` + ctest nonzero-exit semantics (no canary commit required; the wiring is reviewable in the YAML)
   - Expected: reviewer confirms both jobs' steps have no `continue-on-error` and no swallowed exit codes
4. **Matrix untouched** — type: ctest
   - Action: `git diff master... -- .github/workflows/main.yml` inspected at review
   - Expected: existing jobs' blocks unmodified; all 24 pre-existing checks still green on the PR

## Out of Scope
- Any production source or test-code change (fixture upgrade is NEGGIA-004)
- macOS TSan lanes (dev machine covers ad-hoc local TSan; CI TSan is Linux)
- Full-suite Helgrind (too slow for per-PR; scoped to the two concurrency tests)
- Branch-protection/required-checks console settings (human action, recorded in step 5 of the ticket)
- Benchmark/perf lanes (NEGGIA-002 territory, and not CI-wired there either)

## Open Questions
1. Is valgrind preinstalled on ubuntu-24.04 GitHub runners or does the job need `sudo apt-get install -y valgrind`? — audit answers (expected: needs install)
2. Does vendored googletest need a TSan-specific accommodation beyond the existing `CXXFLAGS: -Wno-error=maybe-uninitialized` (NEGGIA-001 commit `2293e9e` precedent)? — audit checks
3. Exact post-build paths of the two test binaries for the valgrind invocation (or invoke via `ctest -R <name>` with `--overwrite MemoryCheck…`? — simpler: direct binary invocation; audit quotes paths from test/CMakeLists.txt)
