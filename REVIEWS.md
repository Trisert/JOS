# REVIEWS.md — review guidelines for RedPill OBSW (JOS)

These conventions define how a **code review of this repository is performed and
reported** — by the in-house reviewer (subagent or human) on any PR, and by any
external reviewer we ask for. They scope the review to **static analysis and
reporting only** — the reviewer must NOT modify code, spawn Cloud Agents, or
offer to open fix PRs.

> History: this file was written as the instruction set for Kilo Code Reviewer.
> Kilo is no longer a gate: the app is still installed on the repository but its
> runs end in `Review failed: The model output limit was reached` with no review
> published, so nothing is gated on it (see `TASKS.md`). The content below
> survives as the review contract we apply ourselves and hand to CodeRabbit.

## Mode
- **Review only.** Report findings as structured comments with severity. Do NOT
  suggest code edits inline, do NOT offer to spawn a Cloud Agent, do NOT propose
  opening a fix PR. The human team applies fixes themselves.
- Keep the verdict explicit: `Request changes` / `Approved` / `Comment`.

## Focus areas (priority order)
1. **Correctness bugs** — undefined behaviour, integer overflow, divide-by-zero,
   uninitialized vars, off-by-one, dead/inert code paths, inverted logic.
2. **Safety / fault tolerance** — fault handlers that block (unbounded busy-wait),
   missing bounds checks, non-atomic writes in exception paths, watchdog false-flags.
3. **QM / space standards alignment** — map findings to:
   - NASA-STD-8739.8 (power-of-ten, static analysis in CI)
   - ECSS-E-ST-40C (software integrity, verification)
   - ECSS-Q-ST-80C (quality assurance)
   - JPL-182 (coding standard, compiler warnings as errors)
   - MISRA C:2025 where applicable
4. **Build / CI hygiene** — flag claims in PR description that do not match the diff;
   unpinned tool versions; scopes that exclude vendored code silently.

## Out of scope
- Vendored code under `JOS/Drivers`, `JOS/Middlewares` (FreeRTOS), `JOS/RadioLib`
  is excluded by design — only flag it if a first-party file misuses it.
- Style nits that do not affect correctness or safety.
- Auto-fix or implementation — reporting only.

## Severity tags
Use 🔴 critical (must fix before merge), 🟠 major (should fix), 🟡 minor (nice to fix),
🟢 positive (note what is correct).
