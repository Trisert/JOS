## What and why

<!-- One task = one worktree + one branch + one PR. Name the TASKS.md row. -->
TASKS.md row: <!-- e.g. 49 -->
Closes #<!-- issue number -->

<!-- What changes, which files, and what fixes the behaviour: cite the document
     (document + section/row/table), not the code alone. -->

## Gates run

Paste the command and its result. A gate that does not apply says why; it is
never silently omitted.

| Gate | Command | Result |
|---|---|---|
| Host tests | `ceedling test:all` | |
| Coverage | `make coverage` (90% line / 75% branch, App path only) | |
| Static analysis | `make cppcheck` / `-canary` | |
| Arm build | `make release` (14.3.Rel1) | |
| Docs / Mermaid | | |
| HIL | | not run: |

## Evidence

<!-- Real output, not prose. For hardware, logs or screenshots. -->

## Not done / limits

<!-- HIL not run, CI not run, a test that only covers one path, a file that is
     excluded from the gates (Core/Src, the two C++ TUs). An unstated limit
     reads as a claim. -->

## Open items

<!-- Anything unresolved goes in THREE places: the module doc, the TASKS.md
     open-decisions list, and this PR body. A silent TODO is a defect. -->
None.

## Checklist

- [ ] `TASKS.md` row updated in this PR, footer `Last update` refreshed
- [ ] No commit on `main`, no self-merge
- [ ] No credential, token, `COMMS_AUTH_KEY` or cookie in the diff, the body or a log
- [ ] New documents/sections cited by name, not by folder
