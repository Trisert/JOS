# JOS documentation strategy — and the 2026-11 full rework

Status: **design approved in chat, spec under review**
Work item: `docs/full-rework` (single PR)
Source snapshot audited: `1a3eb35` (2026-11-10, `origin/main`)

This document is both the design spec for the 2026-11 documentation rework and
the standing strategy for JOS documentation. It is written in English per
`CLAUDE.md` §"Commit messages are usually in Italian. Code, comments and docs are
English only."

---

## 1. Purpose and audience

The documentation set has two distinct readerships, and today it serves only
one of them:

- **Internal** — JOS developers and the SYS/ELE subteams. They need module
  contracts, build/test gates, and the exact boundary between what is
  implemented and what is only specified.
- **External** — people arriving at the public repository: new contributors,
  reviewers, partner teams, and anyone assessing the project. Today they get a
  developer-oriented `README.md` and 23 files with no entry point, no diagram,
  and no map of what is safe to rely on.

The rework makes the second readership work without weakening the first.

**Success looks like:** an external reader can answer *what JOS is, what it runs
on, how the pieces fit, how to build and test it, and how to contribute* within
five minutes and without reading source. An internal reader can still find the
exact contract, the exact gate, and the exact open question.

## 2. Current state — audit at `1a3eb35`

Evidence gathered read-only against `1a3eb35`. Every item below is reproduced by
a command, not recalled.

| # | Finding | Evidence |
|---|---|---|
| A1 | **No image assets at all.** Zero `.svg`/`.png` in the repository. | `find JOS/docs -type f ! -name "*.md"` → empty |
| A2 | **5 Mermaid diagrams total**, all in `arch/README.md` (3), `dev/ci-and-verification.md` (1), `dev/simulation.md` (1). **20 of 23 docs have no diagram.** | `grep -c` on the fence marker, per file. Caution: this row itself contains the literal fence marker inside a code span, so a naive recursive grep over `docs/` over-counts by one; the 5/23 figures exclude this file. |
| A3 | **`docs/README.md` index is incomplete** — 5 files exist but are not named: `api/drivers.md`, `api/ttc-frame.md`, `dev/camera-spi-driver.md`, `dev/scrub.md`, `qual/baseline.md`. | index-vs-tree diff |
| A4 | **Non-English text against the English-only rule.** `dev/scrub.md` (whole document) and `qual/baseline.md` (whole document) are Italian; `api/obsw.md:55` carries one Italian sentence ("Vincolo: il periodo di kick …"). | high-signal marker grep, lines listed |
| A5 | **`dev/scrub.md` documents deleted code.** It describes `App/obsw/scrub.{c,h}`, removed in T1.6; the file itself says "Stato: rimosso". | file body |
| A6 | **`qual/baseline.md` is a stale snapshot.** It reports branch `docs/baseline`, commit `cbd60dd` (**2026-08-24**), and asserts that `origin/main` is the same commit — it is not: `main` is `1a3eb35` (2026-11-10). Every number in it is over two months old, and its central claim of parity with `main` is false. | baseline.md header table; `git log -1 cbd60dd` |
| A7 | **`TASKS.md` row 44 is duplicated** — one `review` row (line 83) and one `in-progress` row (line 87). The `in-progress` row refers to PR #108, which is **merged** since 2026-09-28. | `grep -n '^\| 44 '` |
| A8 | **Uncommitted work on `main`** — `AGENTS.md`, `TASKS.md`, `.gitignore` were modified in the working tree and never committed. `AGENTS.md` and `.gitignore` were recovered into this branch (commit `0af32cc`). The `TASKS.md` hunk was **superseded** by `1a3eb35` (which already carries the newer row 47) and was deliberately not applied, so `TASKS.md` on this branch is byte-identical to `main` until C7 touches it. | `git status` on `main`; `git diff main..docs/full-rework` |
| A9 | **`arch/README.md` self-reports an unreconciled divergence** — `TRIGGER_TASK_COMPLETE` exists in the type definitions but has no completion transition in `state_machine.c`, conflicting with the completed `TASKS.md` entry for PR #74. | arch/README.md, "Operational state model" |
| A10 | **No external entry point.** `README.md` is a developer document; there is no system overview, no "start here", no contribution guide beyond a pointer to `AGENTS.md`. | README.md |
| A11 | **Hardware figures cannot be completed.** The OBC schematic/netlist is absent from the corpus. What *is* recoverable are the gerber net names (OBC V2.0), which fixed the payload SPI, I2C_EXT and I2C_CAM wiring in `1a3eb35`. Only the MCP23017 I2C address remains open. | `1a3eb35` body; `AGENTS.md` |

## 3. Target documentation structure

Two levels, one index. The split is by *reader*, not by topic.

```
README.md                         Public entry point: what JOS is, key figures,
                                  build in 5 minutes, where to go next.
JOS/docs/
├── README.md                     Single index, both levels, every file listed.
├── overview/                     EXTERNAL — new.
│   ├── README.md                 System overview: mission, hardware, buses,
│   │                             what is implemented vs specified.
│   └── contributing.md           How to contribute: workflow, gates, conventions.
├── arch/                         INTERNAL — system design.
│   └── README.md
├── api/                          INTERNAL — module contracts (what exists only).
├── dev/                          INTERNAL — build, CI, hardening, power, camera.
├── qual/                         INTERNAL — verification snapshots.
└── user/                         SEMI-EXTERNAL — operator manual.
```

Rules:

- **`overview/` answers "what and why"; `arch/` and `api/` answer "how, exactly".**
  No contract detail in `overview/`; no mission narrative in `api/`.
- Every file under `docs/` is listed in `docs/README.md` with a one-line purpose.
- `docs/api/<module>.md` keeps the AGENTS.md §7 rule: it describes **only what
  exists**. Planned or specified-but-unimplemented behaviour is not documented
  there; it lives in `arch/` with its unresolved marker, or in `TASKS.md`.
- Unresolved items stay visible in the module doc, the `TASKS.md` open-decisions
  list, and the PR body (AGENTS.md §7). A silent TODO is a defect.

## 4. Figure system

### 4.1 Why a pipeline, not pasted images

Mermaid is already used in the repo and renders on GitHub. But the user needs
figures that also work **outside GitHub** (PDF, print, slides, review comments),
and hand-drawn binaries cannot be reviewed in a diff. The pipeline therefore
keeps **Mermaid as the source of truth** and treats SVG/PNG as build output.

```
docs/figures/
├── src/<id>.mmd          source of truth (reviewed in the diff)
├── dist/<id>.svg         generated — used by docs on GitHub
├── dist/<id>.png         generated — used for PDF/print
├── render.sh             pinned renderer
├── package.json          @mermaid-js/mermaid-cli, exact version + lockfile
└── Makefile              all | clean | check
```

`make check` regenerates into a temp directory and diffs against `dist/`. If the
committed output is stale, the check fails — the same "canary" discipline the
repository already applies to cppcheck and actionlint.

Renderer: `@mermaid-js/mermaid-cli` pinned through `package.json` +
`package-lock.json`, executed with `npx mmdc`. Chromium is required; `render.sh`
uses `PUPPETEER_EXECUTABLE_PATH` when set and otherwise falls back to the system
Chromium. Both SVG and PNG are emitted for every figure.

Docs embed the SVG. The PNG sits next to it for print and is not embedded.

### 4.2 Figure set

Eleven figures. Each is placed where a reader needs it, and each cites its
source of truth in the doc text.

| id | Title | Placed in | Source of truth |
|---|---|---|---|
| `system-context` | System context: GS ↔ OBC ↔ subsystems | `overview/README.md`, `README.md` | `arch/README.md`, SPF V3 Tab. 3.9 |
| `repo-layout` | Repository layout and ownership | `overview/README.md` | repository tree, `AGENTS.md` |
| `boot-sequence` | Boot sequence and policy gates | `arch/README.md` | `Core/Src/main.c` |
| `runtime-tasks` | FreeRTOS tasks, priorities, periods, ownership | `arch/README.md` | `Core/Src/main.c`, `App/*` |
| `state-machine` | Operational state model and transitions | `arch/README.md` | `App/obsw/state_machine.c` |
| `memory-map` | Flash / SRAM / FRAM map with addresses | `arch/README.md` | linker scripts, `mpu.h`, `dual_bank.h` |
| `hardware-boundaries` | Documented vs **unspecified** interfaces | `arch/README.md` | gerber net names, open decisions |
| `ttc-stack` | TT&C frame layout, chunking, uplink authentication | `api/ttc-frame.md` | `App/comms/comms.c` |
| `ci-pipeline` | CI pipeline and gates | `dev/ci-and-verification.md` | `.github/workflows/*` |
| `verification-coverage` | What verification covers — and what it does not | `dev/ci-and-verification.md` | `build.yml`, `TASKS.md` |
| `qualification-status` | Qualification status by requirement area | `qual/status.md` (new) | `TASKS.md` |

### 4.3 Fail-closed drawing rules

The same rule that applies to code applies to figures: an unknown is a refusal,
never a plausible default.

- An interface whose frame format is not specified is drawn with a dashed
  boundary labelled **"not specified in the delivered documents"** — never
  inferred.
- A pin or bus that the corpus does not fix is labelled **"open"** with the
  tracking issue, not filled in.
- A figure never implies a capability the code does not have. Where the code and
  the document set disagree, the figure shows the disagreement, citing both.

## 5. Corrections to apply

All of these land in the same PR.

| # | Correction |
|---|---|
| C1 | Rebuild `docs/README.md` as the complete index of both levels. This spec is itself unindexed until this correction lands — include `dev/documentation-strategy.md`. |
| C2 | Create `docs/overview/` (system overview + contributing guide). |
| C3 | Rewrite `README.md` as the public entry point, with the system-context figure and a 5-minute build path. |
| C4 | **Delete `dev/scrub.md`** (A5) — it documents code that no longer exists. Its history stays in git. |
| C5 | **Replace `qual/baseline.md`.** Either re-measure it at the PR head in English, or delete it and let `qual/status.md` be the single qualification snapshot. If both would coexist, `baseline.md` keeps only what `status.md` does not cover. The chosen option is recorded in the PR body. |
| C6 | Translate or rewrite every non-English passage in `docs/`: the whole of `dev/scrub.md` (removed by C4), the whole of `qual/baseline.md` (handled by C5), and `api/obsw.md:55`. |
| C7 | Fix `TASKS.md` row 44: deduplicate and align the status with PR #108 (merged). Refresh the footer date in the same PR. |
| C8 | Fold the recovered uncommitted changes (`AGENTS.md`, `.gitignore`) into this PR and drop the stash. `TASKS.md` needs no recovery — its stashed hunk was superseded by `1a3eb35` (see A8). |
| C9 | Reconcile A9 (`TRIGGER_TASK_COMPLETE`) — either correct the state-machine description or the `TASKS.md` entry, and record which. |
| C10 | Add figures to the docs listed in §4.2, each with its source citation. |
| C11 | Add a `docs/figures/README.md` explaining how to add a figure and run `make check`. |

### 5.1 Language scope — explicit decision

`CLAUDE.md` requires English for docs. Three places violate it (A4) and will be
fixed: two whole documents (`dev/scrub.md`, `qual/baseline.md`) and one passage
in `api/obsw.md` (C4, C5, C6).

`TASKS.md` is largely Italian and is the **internal work-tracking board**, not
shipped documentation. Translating 36 KB of dense tracking prose is out of
proportion for this rework and would make the diff unreadable. This rework
therefore **does not translate `TASKS.md`**; it fixes its structure (C7) and
leaves its language convention alone. The tension between the CLAUDE.md rule and
the board's convention is recorded here as a known, deliberate exception, and
should be settled by the team separately.

## 6. Deliverable and workflow

- One worktree: `JOS/.worktrees/docs-rework`.
- One branch: `docs/full-rework`.
- **One PR**, as agreed. The audit corrections and the figures reinforce each
  other: a corrected doc without a figure stays hard to read, and a figure on an
  uncorrected doc is damage.
- `AGENTS.md` and `REVIEWS.md` are re-aligned with the repository state but are
  not rewritten; their rules are the contract this PR must satisfy.
- Review follows `REVIEWS.md`; CodeRabbit is triggered explicitly if the
  repository is below the 10★ threshold.

This is a **docs-only** change. No `.c`, `.h`, `.cpp`, `.ioc` or workflow file is
modified. Build, host tests and cppcheck are still run as a sanity check, and
the PR body must state their result — or state that they were not run.

## 7. Acceptance criteria

- [ ] Every file under `docs/` is listed in `docs/README.md`.
- [ ] `docs/overview/` exists and answers the five external questions without
      source reading.
- [ ] Every doc under `docs/` is English.
- [ ] `dev/scrub.md` is gone; no doc describes code that does not exist.
- [ ] `qual/` contains no snapshot older than the PR head.
- [ ] `TASKS.md` has no duplicate row numbers and its footer date matches the PR.
- [ ] All eleven figures from §4.2 exist as `.mmd` sources with generated `.svg`
      and `.png`, and `make check` passes from a clean checkout.
- [ ] Each figure's doc paragraph cites the source of truth it was drawn from.
- [ ] Every figure that touches an unspecified interface carries the
      "not specified" / "open" marker required by §4.3.
- [ ] No firmware, workflow or source file is modified. The only non-doc file
      touched is `.gitignore`, recovered by C8.
- [ ] `make release`, `make test`, `make coverage`, `make cppcheck` results are
      recorded in the PR body (or explicitly marked not run).
- [ ] The PR body lists evidence, unverified items and open questions.

## 8. Non-goals

- No firmware behaviour change, however small.
- No invention of hardware facts that the corpus does not state.
- No translation of `TASKS.md` (see §5.1).
- No re-architecture of the documentation into a generated site (MkDocs, Docusaurus).
- No new CI job. Adding a figure-drift check to CI is valuable but is a workflow
  change and belongs in its own PR; `make check` is the local gate for now.

## 9. Open decisions

1. **`qual/baseline.md` vs `qual/status.md`** — replace in place, or supersede?
   Recorded in the PR body, not decided silently.
2. **Figure-drift CI job** — in this PR or a follow-up?
3. **`TASKS.md` language** — team decision, out of scope here (§5.1).
4. **Should `docs/figures/dist/` be committed?** This spec assumes yes, so the
   figures render for anyone cloning the repo. If the team prefers
   generate-on-demand, `dist/` becomes git-ignored and `make check` runs in CI.
