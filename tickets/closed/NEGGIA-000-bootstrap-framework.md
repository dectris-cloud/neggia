# Ticket: NEGGIA-000 — Bootstrap agent framework

## Status
Closed

## Type
infra

## Priority
P0 — load-bearing for every subsequent NEGGIA-NNN ticket.

## Origin
- Reporter: Max Burian (max.burian@dectris.com)
- Date: 2026-05-29
- Source: User directive in xds session after the XDS-039 + XDS-038 cycle —
  decision to drive Tier-1 perf work in `dectris-cloud/neggia` from the
  same Claude session as XDS-fork work, framework metadata in
  `xds/.claude/`, no separate orchestrator. NEGGIA-NNN ticket numbering;
  3-stage Tier-1 plan to follow.

## Problem Statement
Until this ticket, `dectris-cloud/neggia` had no operational framework
beyond what it ships upstream (its own cmake+ctest suite). Direct code
changes against `main` without a ticket/spec/audit/patch/verify gate
would replicate the failure modes XDS-fork has avoided over its
Phase-0/Phase-1 cycle: scope creep, ABI drift, missed concurrency races,
release notes that don't tell a coherent story.

## Expected Behavior
After this ticket lands:

1. The neggia repo has a `tickets/{open,closed}/` directory tree, a
   `tickets/_template.md`, and a `tickets/README.md` documenting the
   8-step lifecycle.
2. The neggia repo has `docs/{specs,audits,learnings,architecture,plans}/`
   directories.
3. The neggia repo has a `CHANGELOG.md` with a `[Unreleased]` block.
4. The neggia repo has `docs/architecture/agent-framework.md` explaining
   the cross-repo arrangement (framework in `../xds/.claude/`, driven
   from the same Claude session that operates xds).
5. The neggia repo has `docs/abi-baseline.txt` enumerating the four
   `plugin_*` C symbols that constitute the contract with XDS-fork
   (load-bearing for `neggia-surgical-patch`'s ABI HALT).
6. The xds repo has the new agent + 5 skills:
   - `.claude/agents/neggia-archaeologist.md`
   - `.claude/skills/neggia-ticket-intake/SKILL.md`
   - `.claude/skills/neggia-requirement-spec/SKILL.md`
   - `.claude/skills/neggia-deep-audit/SKILL.md`
   - `.claude/skills/neggia-surgical-patch/SKILL.md`
   - `.claude/skills/neggia-verify/SKILL.md`

## Acceptance Criteria
- [x] Directory scaffolding exists in `../neggia/` (verified by `ls`)
- [x] `../neggia/tickets/_template.md` mirrors XDS template with C++/ABI
      adaptations (Type values incl. `perf` and `abi-evolution`; Constraints
      incl. ABI impact + Concurrency surface + Coordinated XDS-fork update
      fields)
- [x] `../neggia/docs/architecture/agent-framework.md` ≥ 80 lines
      explaining the cross-repo arrangement
- [x] `../neggia/docs/abi-baseline.txt` lists exactly the 4 `plugin_*`
      symbols alphabetically
- [x] `xds/.claude/agents/neggia-archaeologist.md` frontmatter
      `tools: Read, Grep, Glob, Bash` (mirror of xds-archaeologist)
- [x] `xds/.claude/skills/neggia-surgical-patch/SKILL.md` declares both
      the 50-line cap (with 1.5× header weight) AND the ABI HALT
- [x] `xds/.claude/skills/neggia-deep-audit/SKILL.md` drives
      `neggia-archaeologist` (not `xds-archaeologist`)
- [x] All 5 skills + 1 agent loaded by Claude Code (verified via the
      skills-available system reminder showing them)

## Constraints
- **Touches:**
  - In xds repo: `.claude/agents/neggia-archaeologist.md` (new),
    `.claude/skills/neggia-*/SKILL.md` (5 new)
  - In neggia repo: `tickets/{open,closed}/`, `tickets/_template.md`,
    `tickets/README.md`, `docs/{specs,audits,learnings,architecture,plans}/`,
    `docs/architecture/agent-framework.md`, `docs/abi-baseline.txt`,
    `CHANGELOG.md` (new), `tools/` (empty for now)
- **ABI impact:** no — this ticket touches no neggia source code.
- **Concurrency surface:** no.
- **Coordinated XDS-fork update needed:** yes — agent + 5 skill files
  authored in xds repo; will be committed there alongside this neggia
  bootstrap commit.
- **Reproducer:** n/a — meta-ticket.

## 8-Step Workflow Log
1. **Ticket:** this file (created 2026-05-29).
2. **Requirement Spec:** waived — bootstrap meta-ticket (per XDS-000
   precedent; spec/audit format itself is what's being established).
3. **Audit & Challenge:** waived — same.
4. **Minimal Patch Proposal:** waived — same.
5. **Apply & Verify:** DONE 2026-05-29. All scaffolding written;
   Claude Code reloaded skills list confirms all 5 neggia-* skills
   plus the agent are registered.
6. **Commit/PR:** TODO — separate commits in each repo
   (xds: agent + skills; neggia: scaffolding + this ticket).
7. **Changelog:** DONE 2026-05-29. `[Unreleased]` block initialised
   with framework-bootstrap note.
8. **Learning:** N/A — no surprises during bootstrap.

## Notes
- Framework conventions mirror XDS-fork's where they transfer
  cleanly (8-step workflow, ticket lifecycle, 50-line cap with
  HALT-on-overflow). Specialised where they don't (C++ archaeology,
  ABI HALT, TSan/Helgrind concurrency gates, bit-exact regression
  vs ndiff).
- Single source of orchestration: this work is driven from the
  xds-repo Claude Code session; no separate session is spawned.
  Skills targeting `../neggia/` use relative paths to operate on
  the sibling repo. The user explicitly requested this single-session
  architecture to avoid context divergence.
- First real exercise of the framework: NEGGIA-001 (Stage 1 of Tier-1
  perf work; worker pool replacing `GLOBAL_HANDLE` singleton).
  Full project plan in `docs/plans/tier-1-performance.md`.
