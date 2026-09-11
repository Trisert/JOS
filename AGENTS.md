# AGENTS.md — working rules for AI agents in this repository

JOS is the **flight** on-board software (OBSW) of the RedPill PocketQube, running
on an STM32L496VGTx. It will fly. A silent bug is not a failed test run, it is a
lost mission, so the default here is **explicit, verifiable, spec-anchored work**
— never "looks right" or "probably fine".

Read this file before your first edit. When it disagrees with the code, the
project documentation and CI win; open an issue instead of silently picking a
side.

---

## 1. Order of authority

1. **The project documentation set on SharePoint, taken as a whole** — SPF v3
   (system specification + operational database), `SW_DATA_TYPES.xlsx`, the OBC
   V2.0 netlist, the published FDIR tables, the ICDs, the BOM, the interface
   documents, the review records. The SPF is **one document inside that set**,
   not a standalone authority, and it is not the newest one by definition.
2. This file and the docs in `JOS/docs/`.
3. The existing code.

**The delivered documents beat the code.** If a value in code disagrees with a
value the documentation set fixes (coding rate, beacon period, frame layout,
threshold, pin), the code is wrong.

**The whole set beats any single document.** If two delivered documents
contradict each other, do not pick the one that suits the change you are making:
flag the contradiction (§7) and escalate it.

**If the documents do not define the interface, stop and ask.** Do not invent a
protocol, a register map, a frame layout, a pin assignment or an endianness.
Interfaces currently in this state (EPS↔OBC frame format, the AOCS↔OBC SPI
transport, the 32-byte PDT header payload) are tracked as blockers at the bottom
of `TASKS.md`. Coding them blind means fabricating an interface that the flight
model will not implement.

**The document set is not in this repo.** Cite document + section/row, never a
recollection of one; if a document you need is missing, ask for it rather than
reconstructing it from the code.

---

## 2. Gates — run them, or say you did not

CI is the authoritative gate. Its jobs are `static-analysis` (cppcheck),
`Firmware build (arm-none-eabi)`, `Host unit tests (Ceedling)`, `Workflow lint
(actionlint)` and `CodeQL`. Everything below is the local equivalent of exactly
what CI runs — there is one definition of each gate, and it lives in
`JOS/Makefile`, never in a workflow `run:` block.

```sh
cd JOS                       # all commands below run from JOS/, not the repo root

make release                 # firmware build; also: make debug | bench | release-strip
make crc-stamp               # stamp the image CRC — REQUIRED before flashing
arm-none-eabi-size build/JOS.elf

make test                    # host unit tests (Ceedling + Unity + CMock)
make coverage                # enforced gate: >=90% line, >=75% branch

make cppcheck                # cppcheck 2.13.0 (pinned; refuses any other version)
make cppcheck-canary         # proves the cppcheck gate can still go red
make cppcheck-includes       # no unresolved first-party #include
make cppcheck-includes-proof # proves that include gate cannot be silenced
make cppcheck-print          # print the fully expanded cppcheck command line
```

- `-fanalyzer` (GCC static analyser, `APP_ANALYZER`) runs on every **first-party**
  C translation unit during a normal build — a clean build is also a clean
  analyzer run for `App/` and hand-written `Core/Src`.
- Workflow edits add one more gate: `actionlint -color` (pinned 1.7.7 in CI).
- Toolchain: CI pins **Arm GNU Toolchain 14.3.Rel1** (the STM32CubeIDE 2.x
  reference) and the Ceedling suite builds with the **host** gcc. Local
  shortcuts differ on purpose — `nix develop` ships gcc-arm-embedded-13 and the
  devcontainer installs the distro `gcc-arm-none-eabi` — so a local build can be
  more or less strict than CI. **CI is authoritative**: a green local build is
  not a substitute for it.

**Reporting rules.** If a tool is not installed, the gate is *not run* — say
"not run", never "passes". Never report CI green without reading the checks of
that head commit, and never report a PR as merged without verifying its state.

**Never silence a gate.** No new `--suppress`, no disabling a check id, no
`paths-ignore`, no `|| true`, no weakened coverage threshold to make a PR green.
If a gate produces a false positive on first-party code, fix the code or fix the
gate *and* add the proof-of-life that shows it can still fail — in the same PR.

---

## 3. Repository map

| Path | What it is |
|---|---|
| `JOS/App/` | **All first-party code.** This is what you edit: `obsw/`, `comms/`, `memory/`, `bms/`, `aocs/`, `payloads/` |
| `JOS/Core/` | CubeMX-generated sources + hand-written glue; `JOS/JOS.ioc` is the pin/peripheral source of truth (subject to §1) |
| `JOS/test/` | Host unit tests (Ceedling), hand-written doubles in `support/`, target-header stand-ins in `fakes/` |
| `JOS/tools/` | `fw_crc_stamp.py`, CRC self-test |
| `JOS/simulation/` | Dual-ESP32 HIL harness (development aid, not flight code) |
| `JOS/docs/` | `api/` module contracts, `dev/` developer guides, `arch/` system design, `qual/` baselines |
| `TASKS.md` | Human-readable source of truth for work tracking (replaces the archived kanban DB) |
| `REVIEWS.md` | The contract Kilo Code Reviewer follows: static analysis + reporting only |
| `JOS/Drivers/`, `JOS/Middlewares/`, `JOS/Core/Inc/RadioLib/` | **Vendored.** STM32 HAL/CMSIS, FreeRTOS, RadioLib headers — never edited, never analysed |

---

## 4. Code rules (embedded + space)

- **C11 (`gnu11`)**, MISRA-oriented, English identifiers **and English comments**.
  Commit messages may be Italian; nothing inside a source file may be.
- **No dynamic allocation after init.** No `malloc`/`free` inside tasks; static
  pools, `heap_4` for init-time only. `configUSE_MALLOC_FAILED_HOOK` is enabled,
  so heap exhaustion is faulted and logged, not ignored.
- **No blocking HAL polling in tasks.** SPI/I2C are interrupt-driven (DMA +
  ISR). `HAL_SPI_Transmit`-style polling calls do not belong in task context.
- **ISRs are short**: flag, copy, notify. No blocking, no logging, no allocation.
- **Every task registers with the watchdog** (`watchdog_register_task()` with its
  expected period) and kicks it (`watchdog_alive()` / `watchdog_alive_self()`);
  an unregistered, silent task is a design error.
- **Single-precision only.** The M4F FPU is single-precision and
  `-Wdouble-promotion` is on; a silent `float → double` promotion falls back to
  soft-float.
- **Declare prototypes** in the module header — implicit declarations are a hard
  error under GCC ≥ 14 even though they only warn on some toolchains.
- **State machine lives only in `App/obsw/state_machine.c`.** Transitions go
  through `try_transition()` / `enter_safe_state()`; no module writes state
  directly.
- **Cross-module contracts are header-only** (`App/*/*.h`) and pinned by tests.
  A module reaches another module through its header, never through a locally
  re-declared prototype or a duplicate static function. (A duplicated
  `bms_get_status()` once made a dead EPS link read as a full battery.)
- **Fail closed.** Unknown telemetry, invalid data or a missing device is a
  refusal, never a default that happens to look nominal.
- In CubeMX-generated files, keep edits inside `/* USER CODE BEGIN */` blocks.
  Peripheral/pin changes belong in `JOS.ioc` **and** must be reconciled with the
  OBC V2.0 netlist; never hand-fix a pin the netlist does not support.
- Compile-time `static_assert` for every spec-pinned size, offset or frame
  length you introduce or touch (beacons, LastStates entries, TT&C frames).
- Cite the source of any magic number in a comment, with document and section:
  `/* SPF §3.7.5.3.1 Tab. 3.28 */`.

### Hardware facts you may rely on

- **MCU** STM32L496VGTx, Cortex-M4F @ 80 MHz; 1 MB Flash (512 KB reserved for
  the image), 320 KB SRAM (256 + 64).
- **LastStates pool** is at absolute address `0x08080000`, 8 KB.
- **FRAM** 512 KB total (4 × FM24VN10-G) on **I2C1** (PB8/PB9) — I2C, not SPI.
- **Radio** Semtech SX1268 on **SPI1** with DMA (DMA1 Ch2/Ch3, request via
  `CSELR`; this L4 has no DMAMUX). I2C2 is the camera bus.
- **Timebase**: HAL timebase on TIM6, SysTick for FreeRTOS only. Do not move
  `HAL_GetTick()` back onto SysTick.
- **AOCS is a separate board with its own MCU**, linked over the subsystem SPI
  as a slave. The OBC does not run attitude control; it consumes the pinned
  telemetry contract.
- **EPS** is a separate MCU board reached over the subsystem SPI (the OBC is
  master, SPI2). Note the documents disagree on the exact part (SPF/docs say
  STM32L1, `App/bms/bms.c` says STM32L496) — do not silently pick one. Its frame
  format is **not specified** — see §1.

---

## 5. Tests

- **Every behavioural change ships with a host test.** Bug fix → a test that
  fails before the fix and passes after. New spec-conformant behaviour → tests
  for the normal path **and** the error/refusal path.
- **Prove the test can fail.** Revert the fix (or mutate the code) locally and
  confirm the new test goes red. A test that cannot fail is not evidence.
- Use the existing doubles (`test/support/`) and header stand-ins (`test/fakes/`).
  Do not add a second Unity or a second mock framework: Unity/CMock come from
  the Ceedling gem only.
- Only host-compilable modules go on the Ceedling source path. `Core/Src` and
  `main.c` never do.
- Coverage is an **enforced** gate (`:fail_under_line: 90`,
  `:fail_under_branch: 75`, `:exception_on_fail: TRUE`). Do not lower it, and do
  not add code that cannot be exercised to dodge it — if a line is genuinely
  unreachable, say why in the review or restructure.
- HIL/ESP32 simulation is a development aid; it never substitutes for the
  Ceedling suite or for a pinned contract test.

---

## 6. Change workflow

```sh
git worktree add .worktrees/<task> -b <type>/<slug> origin/main
```

- **One task = one worktree = one branch = one PR.** Worktrees live under
  `.worktrees/` (git-ignored). Never commit to `main`, never work in the shared
  checkout of another task.
- Branch types: `feat/`, `fix/`, `docs/`, `test/`, `ci/`, `integration/`.
- Commits: Conventional Commits with a module scope —
  `feat(comms): …`, `fix(obsw): …`, `docs(aocs): …`, `test(coverage): …`,
  `ci(codeql): …`. Scopes in use: `obsw`, `comms`, `bms`, `aocs`, `memory`,
  `payloads`, `ci`, `test`, `docs`, `tasks`.
- **PR body must contain**: what changed and why; the document reference (§1) it
  implements; the exact commands you ran and what they returned; anything you
  did **not** verify; and any open question or contradiction you hit.
- Stacked PRs are allowed and gated (CI has no base-branch filter).
- **Do not merge your own PR.** Merge happens on explicit human approval.
- Reviews: **Kilo Code Reviewer** follows `REVIEWS.md` (reporting only, no
  auto-fix). **CodeRabbit** does not auto-review this repo automatically — ask
  for it with an `@coderabbitai review` comment when you want a second pass.
- Every PR that touches flight behaviour earns a reviewer verdict **before**
  it is merged — an approval you cannot attribute to a specific review of that
  head commit is not a verdict.
- Licensing: `radiolib_driver.cpp` / `radiolib_hal.cpp` adapt code from
  RedPill-T (`license: null`). Do not copy more code from it and do not "solve"
  that blocker yourself; it is an explicit grant question for the team.

---

## 7. Documentation duties

- `docs/api/<module>.md` describes **only what exists**. Never document a
  function that does not exist, and never mark something "integrated" because it
  is planned. Documentation drift here is a defect with a PR of its own.
- `docs/dev/*` (building, hardening, simulation, coding standards) is updated in
  the same PR as the behaviour it describes.
- `TASKS.md` is the work-tracking source of truth: add/update the row for your
  task and refresh the footer date in the same PR.
- **Unresolved** items stay visible: annotate them in the module doc and in the
  `TASKS.md` "open decisions" list, and name them in the PR body. That is the
  opposite of a silent TODO.
- Contradictions inside the SharePoint set (two documents disagreeing on a value,
  a part, a priority) are recorded here or in `TASKS.md` with both readings and
  both sources — never resolved by picking one.

---

## 8. Definition of done

A change is done when **all** of these hold:

- [ ] the firmware builds (`make release`, `make crc-stamp` succeeds)
- [ ] host tests pass and coverage is above the enforced thresholds
- [ ] cppcheck (and actionlint, if workflows changed) is clean, with no new
      suppression
- [ ] new behaviour has tests, and those tests were shown to fail without the
      change
- [ ] every pinned number in the diff cites its document and section
- [ ] `docs/` and `TASKS.md` reflect reality
- [ ] the PR body lists evidence, unverified items and open questions
- [ ] CI is green on the exact head commit that is proposed for merge

If a gate could not be run, say so in the PR body. An honest "not verified" is
acceptable; a confident claim without evidence is not.
