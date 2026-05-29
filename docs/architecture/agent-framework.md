# Agent framework — cross-repo arrangement

This repo (`dectris-cloud/neggia`) is operated under an agent framework
whose metadata lives in a **sibling** repo: `../xds/.claude/`. There is
**no `.claude/` directory in this repo by design** — the framework is
maintained in one place (xds), and from a single Claude Code session
running in xds, work is driven in both xds and neggia.

This document explains the arrangement so future readers understand why
the framework looks split.

## Directory layout (relative paths)

```
03_Programming_Projects/
├── xds/                    ← XDS-fork; primary working directory of the Claude session
│   ├── .claude/
│   │   ├── agents/
│   │   │   ├── xds-archaeologist.md       (Fortran archaeology)
│   │   │   └── neggia-archaeologist.md    (C++ archaeology, targets ../neggia/)
│   │   └── skills/
│   │       ├── xds-*/                     (XDS workflow skills)
│   │       └── neggia-*/                  (neggia workflow skills, target ../neggia/)
│   ├── src/                               (XDS Fortran source)
│   ├── tickets/{open,closed}/             (XDS-NNN tickets)
│   ├── docs/{specs,audits,learnings,architecture,plans}/  (XDS docs)
│   └── CHANGELOG.md
│
└── neggia/                 ← THIS repo; sibling
    ├── src/dectris/neggia/                (C++ source — plugin/user/data/...)
    ├── tickets/{open,closed}/             (NEGGIA-NNN tickets — this dir tree)
    ├── docs/{specs,audits,learnings,architecture,plans}/  (NEGGIA docs)
    ├── tools/                             (neggia-side scripts as needed)
    ├── CHANGELOG.md
    └── (NO .claude/)                      (deliberately; framework lives in ../xds/.claude/)
```

## Why this arrangement (not separate frameworks)

Three reasons:

1. **One conversation, one operator.** The XDS-fork side is the
   downstream consumer of neggia via the four `plugin_*` C symbols
   (`xds/src/generic_data_plugin.f90:140-170`). Many neggia changes
   produce coordinated XDS-fork ticket pairs (e.g. bumping
   `xds/tools/neggia-version.txt` after a neggia release). A single
   operator with both contexts in hand makes those coordinations
   cleanly; a split would require duplicating context across
   sessions.
2. **Single source of truth for the workflow.** The 8-step ticket
   workflow, the 50-line cap discipline, the audit-before-patch gate,
   and the verify-before-commit gate are all properties of the
   framework, not of either codebase. Defining them once in
   `xds/.claude/` keeps them DRY.
3. **Language specialisation, framework reuse.** The
   `neggia-archaeologist` agent and `neggia-surgical-patch` skill
   carry C++-specific patterns (clangd probes, threading-model
   analysis, ABI surface inspection, TSan/Helgrind gates) that the
   XDS Fortran skills don't need. They live alongside the XDS skills
   in the same `.claude/` directory but operate on `../neggia/`
   paths — the relative path is the only thing that distinguishes
   the targets.

## Hard rules from the XDS-fork framework that ALSO apply here

1. **Every change traces to a ticket.** `NEGGIA-000` bootstraps the
   framework; everything after must have a ticket.
2. **Audit before patch, patch before verify, verify before commit.**
   No skipping steps.
3. **50-line surgical cap** (with 1.5× header weight on `.h`/`.hpp`).
4. **ABI sacred.** The four `plugin_*` C symbols
   (`plugin_open`/`plugin_close`/`plugin_get_header`/`plugin_get_data`)
   must keep their signatures unless the ticket type is
   `abi-evolution` AND a coordinated XDS-fork ticket exists to update
   the Fortran caller.
5. **Humans apply diffs.** Agents propose; skills orchestrate; humans
   review and commit.

## What's different from XDS-fork

- **We own this repo.** There is no "upstream sacred" branch
  equivalent to `upstream/kabsch`. The Hard Rule 4 from XDS-fork (no
  edits to upstream-sacred paths) doesn't apply here — what's
  preserved instead is the ABI surface (above) and bit-exactness on
  the data path.
- **Build is cmake + ctest** (vs `cat_source` + per-platform
  makefiles + pFUnit on XDS).
- **Validation is bit-exact + TSan + Helgrind** (vs ndiff against
  goldens on XDS).
- **Release versioning is semver** (`v1.x.y` — vs XDS's CalVer
  `vYYYY.MM`). Semver because neggia release cadence is feature-driven
  (e.g. "Tier 1 perf changes shipped"), not time-driven like XDS.
- **Scientists-in-cloud validation** for perf claims. After each
  neggia release that touches the data path, Dectris-cloud
  scientists run real benchmarks on GeeseFS-S3 + 192-core hardware
  and record results in `CHANGELOG.md` and (where appropriate)
  per-release validation docs under `docs/learnings/`.

## How a session works in practice

The operator (currently: Max Burian via Claude in the xds repo session):

1. Says "drive NEGGIA-001" (or similar trigger).
2. The Claude session uses the `neggia-*` skills (visible alongside
   `xds-*` skills) to walk the 8-step workflow.
3. Skills targeting `../neggia/` read+write under that path; skills
   targeting xds paths read+write under `xds/`. The session manages
   both.
4. Outputs (tickets, specs, audits, code edits) land in their proper
   repo and are committed via the operator's git commands.

Reading order for new framework contributors: this doc → `xds/CLAUDE.md`
→ `xds/docs/architecture/agent-framework.md` → the `neggia-*` skill
bodies under `xds/.claude/skills/`.
