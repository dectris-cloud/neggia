# Ticket: NEGGIA-NNN — <one-line title>

## Status
Open | In Review | Closed

## Type
bug | enhancement | perf | refactor | infra | docs | abi-evolution

## Priority
P0 (release blocker) | P1 (next release) | P2 (when time permits) | P3 (icebox)

## Origin
- Reporter:
- Date:
- Source: <user report | benchmark finding | XDS-fork audit | other>

## Problem Statement
<What is the current behavior? Why is it a problem?>

## Expected Behavior
<What should happen instead? Be quantitative if at all possible — throughput,
latency, line count, ABI surface, etc.>

## Acceptance Criteria
- [ ] Specific testable condition 1 (ctest case name; TSan-clean assertion;
      bit-exact regression; benchmark threshold; ABI parity check)
- [ ] Specific testable condition 2

## Constraints
- Touches: `src/dectris/neggia/<files>` | `test/<files>` | none
- ABI impact: yes (plugin_* signature change — requires
  `abi-evolution` type) | no
- Concurrency surface: yes (threading state touched — requires TSan +
  Helgrind gates per spec) | no
- Coordinated XDS-fork update needed: yes (`xds/src/generic_data_plugin.f90`
  + `xds/tools/neggia-version.txt`) | no
- Reproducer: <command, fixture, or "n/a">

## 8-Step Workflow Log
1. **Ticket:** this file (created <date>)
2. **Requirement Spec:** `docs/specs/NEGGIA-NNN.md` (link)
3. **Audit & Challenge:** `docs/audits/NEGGIA-NNN.md` (link)
4. **Minimal Patch Proposal:** <human-approved diff — embedded in audit doc>
5. **Apply & Verify:** <build/test result summary, link to log>
6. **Commit/PR:** <commit hash(es), PR link if applicable>
7. **Changelog:** `CHANGELOG.md` entry under `[Unreleased]` → <section>
8. **Learning:** `docs/learnings/NEGGIA-NNN.md` (link, optional)

## Notes
<Free-form discussion, decisions, references>
