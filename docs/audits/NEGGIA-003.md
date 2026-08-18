# Audit: NEGGIA-003 — TSan + Helgrind CI lanes

## Spec Reference
../neggia/docs/specs/NEGGIA-003.md

## Symbol / File Inventory
(from neggia-archaeologist, 2026-08-18)

- `.github/workflows/main.yml` (113 lines): trigger `on: [push, pull_request]`; jobs `posix-cc` (2 build-types × 4 OS = 8) + `posix-gcc` (4 includes) = **12 jobs/event → 24 check runs on a PR** (push + pull_request). Checkout `actions/checkout@v4` with `submodules: true`. Configure: `cmake ${{github.workspace}} -DCMAKE_BUILD_TYPE=... -DDEBUG_PARSING=... -DCMAKE_POLICY_VERSION_MINIMUM=3.5` in sibling dir `${{github.workspace}}.build`. Job-level `env: CXXFLAGS: -Wno-error=maybe-uninitialized` (vendored-gtest accommodation on gcc-11+). Test: `ctest --output-on-failure`. Per-lane `.so` artifact upload.
- NEGGIA-001's actual TSan recipe (`docs/audits/NEGGIA-001.md:579-587`, confirmed against live `build-tsan/CMakeCache.txt`): **Release** + `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` + `-DCMAKE_C_FLAGS="-fsanitize=thread -g"`.
- Test binaries (`test/CMakeLists.txt:68,93`): `<build>/src/dectris/neggia/test/Test_XdsPlugin` and `.../Test_XdsPluginConcurrent`; tests resolve fixtures via a **relative** path through the build-dir symlink (`test/CMakeLists.txt:3-7`) → direct invocation must run with cwd = that test dir. `PATH_TO_XDS_PLUGIN` is an absolute compile definition → dlopen is cwd-independent.
- valgrind: **NOT preinstalled** on ubuntu-22.04/24.04 GitHub runners → `sudo apt-get install -y valgrind` required (spec OQ1 resolved).

## Threading Model
No production threading changed — this ticket only *observes* the existing worker-pool concurrency. TSan instruments both C++ and C objects (bitshuffle.c/lz4.c compile into the same `.so` via `NEGGIA_COMPRESSION_ALGORITHMS` — **`CFLAGS`/`-DCMAKE_C_FLAGS` is load-bearing, not optional**). dlopen'ing a TSan-instrumented `.so` from a TSan-instrumented test binary is the supported configuration (proven by the local build-tsan run).

## Call Graph
Workflow-level only: two new jobs consuming the existing CMake/ctest graph. No source symbol touched.

## ABI Surface Impact
**NONE.** Zero source diff (spec invariant 1). ABI HALT does not trigger.

## Invariants
1. Existing 12-job matrix byte-untouched (spec inv 2)
2. **Flag-delivery pitfall (the audit's key finding):** the gtest accommodation arrives via the `CXXFLAGS` *environment variable*, which CMake uses only to seed `CMAKE_CXX_FLAGS` when it is NOT given on the command line. Passing `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` **silently drops `-Wno-error=maybe-uninitialized`** → gcc-13 build break in vendored gtest. The TSan job must deliver flags via env: `CXXFLAGS: -fsanitize=thread -g -Wno-error=maybe-uninitialized` + `CFLAGS: -fsanitize=thread -g`, with no `-DCMAKE_*_FLAGS` on the configure line (spec OQ2 resolved).
3. **TSan build type = Release** — resolves the ticket-text "Debug" vs recipe discrepancy deliberately: Release reproduces the only existing TSan evidence (NEGGIA-001's local run) and catches optimizer-dependent interleavings; `-g` in the flags keeps reports symbolized. Recorded here as the spec-level decision (amends spec inv 3's "Debug" wording).
4. TSan failure propagation: a process with TSan reports exits 66 by default → ctest fails → job fails; add `TSAN_OPTIONS: halt_on_error=1` (one env line) for immediate first-failure diagnostics. The log-grep AC remains as the reviewer check, not a workflow step (cap economy).
5. Helgrind lane: Debug build (CMake default Debug = `-g`) for symbolization; `valgrind --tool=helgrind --error-exitcode=1` on the two binaries, invoked with cwd = `<build>/src/dectris/neggia/test` (relative-fixture trap, spec OQ3 resolved)
6. Wall-time ≤15 min holds for the current 5-frame fixture; **re-check when NEGGIA-004 merges** (whichever of 003/004 lands second inherits the combined signal — Large001 under Helgrind is the cost driver; NEGGIA-004's audit caps the tested frame range for exactly this reason)

## Risks
- **Medium**: (i) flag-delivery pitfall (invariant 2) — a lane that accidentally configures without TSan flags would pass *vacuously green*; mitigation: the job echoes the effective `CMAKE_CXX_FLAGS` from CMakeCache into the log (one grep-able line, reviewer-verifiable); (ii) 40-46 projected lines vs 50 cap — tight; comment lines excluded from cap but kept minimal anyway; (iii) valgrind version on ubuntu-24.04 (3.22) vs glibc — no known issue for this workload.

## Existing ctest Coverage on Surface
All 9 ctest cases run in the 12 existing lanes — but never under TSan or Helgrind. The two concurrency-relevant cases (`Test_XdsPluginConcurrent`, `Test_XdsPlugin`) have TSan evidence only from one local run on the 5-frame synthetic fixture; Helgrind evidence: none, ever (NEGGIA-001 AC-5 deferral — this ticket closes it; first green run URL goes into the closed ticket's Notes per spec inv 7).

## Challenge Questions Answered
- valgrind preinstalled? No — apt install needed (OQ1).
- gtest TSan accommodation? No *new* one needed; the existing one is silently lost via `-DCMAKE_CXX_FLAGS` — env delivery mandatory (OQ2).
- Binary paths + invocation? Quoted above; cwd trap resolved (OQ3).
- Why not full-suite Helgrind? Cost; the 7 non-concurrent tests exercise no threads — Helgrind on them observes nothing the plain lanes don't.

## Devil's Advocate
- **Strongest argument against:** a vacuously-green TSan lane is worse than no lane — if the flag plumbing regresses (say a future workflow edit moves flags to the configure line), the lane keeps passing while instrumenting nothing, and the framework's concurrency gate becomes theater precisely when NEGGIA-006/007 depend on it.
- **Resolution:** the lane self-proves instrumentation: one step greps `CMakeCache.txt` for `-fsanitize=thread` and fails if absent (2 lines, inside cap). With that, "green" implies "instrumented".
- **Second-order effects:** none on the `.so` shipped to users (lanes are additive, artifacts come from the existing jobs); CI minutes +~6-10/PR.
- **What would make this audit wrong:** if TSan's runtime intercepted dlopen'd-library races only with additional flags — contradicted by the local build-tsan evidence on this exact test; if GitHub runner valgrind couldn't handle gcc-13 Debug DWARF — valgrind 3.22 handles DWARF5; low.

## Cap-Unit Projection (sum-counted)
Workflow additions ~44-48 lines (two jobs: TSan ~20 incl. cache-grep guard, Helgrind ~26 incl. apt install + 2 invocations + cwd handling); no removals; no headers. **Inside the 50 cap with little slack** — comments minimal; if the draft overflows, drop the Helgrind `Test_XdsPlugin` invocation (lowest-value line) before anything else.

## Audit Verdict
- **READY_FOR_PATCH**
- Rationale: scope matches the spec (one workflow file; the contingent ≤5-line CMake plumb proved unnecessary — env delivery suffices); all 3 OQs resolved; both sharp edges (flag-delivery pitfall, vacuous-green) have concrete in-cap mitigations; the Release-vs-Debug discrepancy is resolved deliberately in favor of reproducing NEGGIA-001's evidence.

## Minimal Patch Proposal

Source-line count: **49 / 50** (additions: 49 non-comment/non-blank YAML, removals: 0; comment/blank tracked-excluded: 4; zero lines under `src/dectris/neggia/`; no headers; CI workflows cap-bound per CLAUDE.md Hard Rule 3).

### Diff
Two additive jobs appended to `.github/workflows/main.yml` on `feature/NEGGIA-003-tsan-helgrind-lanes` (whole-diff visible in the PR; existing 12-job matrix byte-untouched — verifiable via `git diff master... -- .github/workflows/main.yml` showing pure append):
- **tsan** (ubuntu-24.04): flags via `env: CXXFLAGS/CFLAGS` (audit invariant 2 — `-D` would drop the gtest accommodation), Release + `-fsanitize=thread -g` (reproduces NEGGIA-001's evidence, audit invariant 3), `TSAN_OPTIONS: halt_on_error=1`, **instrumentation-proof step** (`grep -- '-fsanitize=thread' CMakeCache.txt` — Devil's-Advocate vacuous-green mitigation), full ctest.
- **helgrind** (ubuntu-24.04): `apt-get install valgrind` (not preinstalled — OQ1), Debug build for symbolization, `valgrind --tool=helgrind --error-exitcode=1` on `Test_XdsPluginConcurrent` + `Test_XdsPlugin` with cwd = the test dir (fixture-symlink trap — OQ3).

### Per-hunk justification
1. tsan job — spec inv 3 (TSan contract) + inv 4 amendment (Release decision) + Devil's-Advocate instrumentation proof
2. helgrind job — spec inv 4 (Helgrind contract), inv 5 (≤15 min: two small binaries only)
3. No CMake change needed (spec's contingent ≤5-line plumb unused); CHANGELOG deferred to post-merge sweep

### Verification plan
- Self-verifying: both jobs run on this PR — TSan lane green with the instrumentation-proof step passing; Helgrind lane exit 0 on both binaries within budget
- Matrix untouched: 24 pre-existing checks still green on the PR
- Post-merge: first green Helgrind run URL on master → append to `tickets/closed/NEGGIA-001-*.md` Notes §Closeout (spec inv 7, closes the AC-5 deferral)
- Required-checks/branch-protection update: human console action, record in ticket step 5

### Verdict
- **READY_FOR_HUMAN_APPLY** (merge = apply; the PR's own lanes are the verification)
