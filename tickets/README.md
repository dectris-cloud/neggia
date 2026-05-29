# Tickets — NEGGIA-NNN lifecycle

Every change to neggia source traces to a ticket here (mirror of XDS-fork's
convention). The framework that drives these tickets lives in
`../xds/.claude/` (one directory up, sibling repo) — see
`docs/architecture/agent-framework.md` for the cross-repo arrangement.

## Lifecycle

1. **Open** — `tickets/open/NEGGIA-NNN-<slug>.md` — created by the
   `neggia-ticket-intake` skill.
2. **In Review** — patch proposed and audit verdict `READY_FOR_PATCH`;
   PR open against `main`.
3. **Closed** — `tickets/closed/NEGGIA-NNN-<slug>.md` — moved here when
   PR merged AND CHANGELOG `[Unreleased]` entry exists AND optional
   learning doc landed (if surprising).

## ID rules

- Monotonically increasing 3-digit zero-padded integers starting at
  `NEGGIA-000` (the bootstrap meta-ticket).
- Never reuse. Never renumber. Gaps stay.
- One ticket = one logical change. The `neggia-surgical-patch` skill's
  50-line cap (with 1.5× header weight) is the load-bearing rule.

## Slug rules

- `kebab-case`, ≤6 words, derived from the title.
- No spaces, no underscores, no leading numbers.

## Workflow

8 steps per `xds/.claude/skills/neggia-*/SKILL.md`:

1. **Ticket** (`neggia-ticket-intake`)
2. **Spec** (`neggia-requirement-spec`)
3. **Audit** (`neggia-deep-audit` → `neggia-archaeologist`)
4. **Minimal patch** (`neggia-surgical-patch` — HALTs at 50-line cap;
   HALTs on `plugin_*` ABI change in non-`abi-evolution` ticket)
5. **Apply & verify** (`neggia-verify` — cmake build + ctest + TSan +
   Helgrind + bit-exact regression as the spec declares)
6. **Commit / PR** — human action
7. **Changelog** — append to `CHANGELOG.md` `[Unreleased]`
8. **Learning** — optional `docs/learnings/NEGGIA-NNN.md`
