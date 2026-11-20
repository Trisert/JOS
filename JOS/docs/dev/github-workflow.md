# Issues and project workflow

How JOS work is tracked on GitHub. This file describes the *surface*;
`TASKS.md` remains the human-readable source of truth for **what** is being
done, and `AGENTS.md` remains the authority for **how** it is done.

## Where each fact lives

| Surface | Role | Authority |
|---|---|---|
| `TASKS.md` | task rows, open decisions, blockers | source of truth for intent |
| GitHub issues | live per-item state, evidence, discussion | source of truth for current state |
| Pull requests | the change, the gates, the merge record | source of truth for what landed |
| Project **JOS** | cross-cutting view of issues and PRs | derived, never authoritative |

The `TASKS.md` status column is a claim written by hand, so it lags. Before
answering any status question, cross-check `gh pr list --state all`,
`gh issue list --state all` and `gh run list`.

The rule that keeps the two in sync: **every item with a code impact gets a
`TASKS.md` row, and the row is updated in the same PR that lands the work.**
An issue is not a substitute for a row, and a row is not a substitute for the
issue thread where the evidence accumulates.

## Issue forms

Blank issues are disabled. `.github/ISSUE_TEMPLATE/` carries three forms:

| Form | Use it for | Required evidence |
|---|---|---|
| `task.yml` | a unit of work from `TASKS.md` | scope with a document citation, acceptance criteria, gates that apply |
| `decision.yml` | a question the repository cannot answer: conflicting documents, or an interface the corpus does not define | one line per source, impact if unanswered, interim fail-closed behaviour |
| `bug.yml` | a defect in firmware, tests, CI or documentation | `file:line` at a commit, the document that fixes the value, a reproduction |

`decision.yml` exists because `AGENTS.md` §1 forbids picking a side when two
delivered documents disagree. The open EPS and AOCS interfaces are tracked this
way, not as ad-hoc notes.

## Labels

Labels are the only taxonomy the project reads. Prefixes are flat, because
GitHub has no label hierarchy.

| Label | Meaning |
|---|---|
| `task` | planned work with a `TASKS.md` row |
| `decision` | needs an answer from SYS, ELE or the ground segment |
| `hil` | cannot be closed without hardware in the loop |
| `area/firmware` | `App/` and hand-written `Core/` |
| `area/comms` | TT&C: RadioLib/SX1268, beacon, frames, uplink authentication |
| `area/eps` | EPS interface: BMS, SoC gates, SPI slave link |
| `area/aocs` | AOCS interface and telemetry contract |
| `area/payloads` | CRYSTALS / CLOUD / CLEAR payload paths and scheduler |
| `area/ci` | workflows, toolchains, gates |
| `prio/P0` | wrong flight behaviour, or a silently dead gate |
| `prio/P1` | wrong result in a non-flight path, or a blocker for qualification |
| `prio/P2` | opportunistic, docs polish |
| `status/blocked` | waiting on a dependency (hardware, document, decision) |
| `status/needs-sys-ele` | awaiting confirmation from the SYS or ELE subteam |

Default GitHub labels (`bug`, `documentation`, `question`, ...) stay. The
legacy catch-all `jos` label is gone: the repository is JOS, so it never
carried information.

Label colours are duplicated in `scripts/github/sync-labels.sh` so the set is
reproducible on a fork or after a mistake.

## Project fields

The **JOS** project (owner `Trisert`, type *project*) has four fields:

| Field | Type | Values |
|---|---|---|
| Status | built-in | Todo, In progress, In review, Blocked, Done |
| Area | single select | Firmware, COMMS, EPS, AOCS, Payloads, CI, Docs |
| Priority | single select | P0, P1, P2 |
| TASKS.md row | text | the row number, so the card can be traced back to the board |

Status is a view of reality, not an input: it should follow the PR state
(`In progress` while the branch is open, `In review` from PR open to merge,
`Done` when the item closes).

GitHub's *auto-add* and *built-in* project workflows are configured in the web
UI only. There is no API for them, so re-check them after creating a project by
hand: auto-add issues from this repository, and keep closed items from
re-entering `Todo`.

## Lifecycle

1. Open the issue with the form. It lands with `task` / `decision` / `bug`,
   plus `needs-triage` handling in the project.
2. `git worktree add .worktrees/<slug> -b <type>/<slug> origin/main`.
   `<type>` is `feat`, `fix`, `docs` or `chore`; one worktree, one branch, one
   PR, never a commit on `main`.
3. Work, then open the PR with `Closes #<issue>` and the gate evidence in the
   body. Missing gates are named with the reason, not omitted.
4. The same PR updates the `TASKS.md` row and refreshes its `Last update`
   footer line.
5. Merge only on explicit human approval with green in-gate checks
   (`REVIEWS.md`), then move the project card to `Done`.

## Security

Issues and PRs are public. Never paste a credential, token, uplink
`COMMS_AUTH_KEY`, SharePoint cookie or device code into an issue, a PR body, a
comment or a log. Reference the vault entry or the environment variable by name
instead.

## Reproducing the label set

```bash
./scripts/github/sync-labels.sh          # idempotent, needs the `repo` scope
```

Creating or editing project items needs the `project` scope
(`gh auth refresh -s project`). The command is interactive by design: it prints
a one-time code that has to be entered by the account owner.
