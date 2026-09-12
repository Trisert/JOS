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
protocol, a register map, a frame layout, a pin assignment or an endianness. The
interfaces currently in this state — the **EPS↔OBC frame format** and the
**AOCS↔OBC frame format** (their *transports* are documented: subsystem SPI, OBC
master, both the other boards as slaves, chip-selects through an I2C GPIO
expander) — are tracked as blockers at the bottom of `TASKS.md`. Coding them
blind means fabricating an interface that the flight model will not implement.

**The document set is not in this repo.** Cite document + section/row/table,
never a recollection of one; if a document you need is missing, ask for it rather
than reconstructing it from the code.

---

## 2. Gates — run them, or say you did not

CI is the authoritative gate. Its jobs are `static-analysis` (cppcheck),
`Firmware build (arm-none-eabi)`, `Host unit tests (Ceedling)`, `Workflow lint
(actionlint)` and `CodeQL`. The commands below are what those jobs actually run,
so a local run and CI cannot drift:

- **Makefile-owned** — the firmware build and the entire cppcheck gate. CI only
  ever calls `make -C JOS …`, so there is exactly one definition of them.
- **Workflow-owned** — the Ceedling suite and the coverage gate (CI runs
  `ceedling test:all` and `ceedling gcov:all` in `JOS/test`; `make test` and
  `make coverage` wrap those same commands) and `actionlint -color` (installed
  and invoked by the workflow). When you change one of these, read
  `.github/workflows/build.yml` — the Makefile is not the authority for it.

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

- `-fanalyzer` (GCC static analyser, `APP_ANALYZER`) is applied to
  `$(BUILD_DIR)/App/%` and `$(BUILD_DIR)/Core/Src/%` only. It is **C-only**: the
  two C++ translation units (`App/comms/radiolib_driver.cpp`,
  `radiolib_hal.cpp`) are outside it, and so is everything under `JOS/tools/`
  and `JOS/simulation/`. A clean build is not coverage of those files.
- Scope caveat worth knowing: the analyser also runs over the few CubeMX/ST
  generated files inside `Core/Src` (`system_stm32l4xx.c`, `stm32l4xx_it.c`,
  `stm32l4xx_msp.c`, `freertos.c`), so its output is not a pure first-party
  signal — but it is always warnings-only, never `-Werror`.
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
If a gate produces a false positive, fix the code or fix the gate *and* add the
proof-of-life that shows it can still fail — in the same PR.

---

## 3. Repository map

| Path | What it is |
|---|---|
| `JOS/App/` | **Primary application code** — the modules you edit: `obsw/`, `comms/`, `memory/`, `bms/`, `aocs/`, `payloads/` |
| `JOS/Core/` | CubeMX-generated sources **plus hand-written glue**; the hand-written part is project code, the generated part is only edited inside `/* USER CODE BEGIN */` blocks. `JOS/JOS.ioc` holds the pin/peripheral configuration (subject to §1) |
| `JOS/test/` | Host unit tests (Ceedling), hand-written doubles in `support/`, target-header stand-ins in `fakes/` |
| `JOS/tools/` | Project-owned helper sources — `crc_selftest.c`, `fw_crc_stamp.py` (outside the gate scope, see below) |
| `JOS/simulation/` | Project-owned dual-ESP32 HIL harness (host/Target code, development aid — not flight code, and outside the gate scope) |
| `JOS/docs/` | `api/` module contracts, `dev/` developer guides, `arch/` system design, `qual/` baselines |
| `TASKS.md` | Human-readable source of truth for work tracking (replaces the archived kanban DB) |
| `REVIEWS.md` | The code-review contract this repo applies on every PR — severity, focus areas, standards mapping. Reporting only, no auto-fix |
| `JOS/Drivers/`, `JOS/Middlewares/`, `JOS/Core/Inc/RadioLib/` | **Vendored**, third-party. STM32 HAL/CMSIS, FreeRTOS, RadioLib headers — never edited, never analysed |

**Ownership and analysis scope are two different sets — do not conflate them:**

- **Owned by this repo** (tracked, maintained here, never treated as vendored):
  `JOS/App/`, the hand-written `JOS/Core/Src` glue, `JOS/test/`, `JOS/tools/`,
  `JOS/simulation/`.
- **Covered by the firmware gates** (cppcheck `CPPCHECK_SRC` and `-fanalyzer`
  in `JOS/Makefile`): `App` and `Core/Src` only. `JOS/tools/` and
  `JOS/simulation/` are owned but **outside** that scope, and so are the two
  C++ TUs. Never read a clean gate as coverage of files it does not compile.

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
- Compile-time `static_assert` for every size, offset or frame length a document
  fixes and you introduce or touch (beacons, LastStates entries, TT&C frames).
- Cite the source of any magic number in a comment, with document and section:
  `/* SPF §3.7.5.3.1 Tab. 3.28 */`.

### Hardware and interface facts you may rely on

- **MCU** STM32L496VGTx, Cortex-M4F @ 80 MHz; 1 MB Flash (512 KB reserved for
  the image), 320 KB SRAM (256 + 64).
- **LastStates pool** is at absolute address `0x08080000`, 8 KB.
- **FRAM** 512 KB total (4 × FM24VN10-G) on **I2C1** (PB8/PB9) — I2C, not SPI.
- **Radio** Semtech SX1268 on **SPI1** with DMA (DMA1 Ch2/Ch3, request via
  `CSELR`; this L4 has no DMAMUX). I2C2 is the camera bus.
- **Timebase**: HAL timebase on TIM6, SysTick for FreeRTOS only. Do not move
  `HAL_GetTick()` back onto SysTick.
- **Subsystem SPI bus**: the OBC is master; **AOCS and EPS are slaves on that
  same bus**, each with a dedicated chip-select. The CS lines are **driven by an
  external GPIO expander over I2C** (`I2C_EXT`), not by native OBC GPIOs, and
  the subsystem MCUs raise asynchronous events on `INT1`/`INT2`
  (`ELE_DREP_Architecture_V01`, SPF V3 Tab. 3.9).
- **AOCS is a separate board with its own MCU**, linked over the subsystem SPI
  as a slave; the physical link is the Y-sliding plate with pogo-pins. The OBC
  does not run attitude control; it consumes the pinned telemetry contract.
- **EPS** is a separate MCU board on the same subsystem SPI. The documents
  disagree on the part number — SPF V3 Tab. 3.9 says `STM32L496VGT3` ("the three
  primary subsystem boards … are all built around the same STM32L4 family"),
  while an older paragraph of the SPF and `SW_DREP` say STM32L1 (and
  `docs/arch/README.md` repeats L1). **Do not silently pick one**: it is open
  decision 5 in `TASKS.md`.
- **Beacons are 128 B**: 96 B of sensor telemetry + 32 B of timestamp/system
  parameters (SPF V3). The "32-byte PDT header" does not exist — `PDT` is an
  operational phase of s4, not a packet format.

---

## 5. Tests

- **Every behavioural change ships with a host test.** Bug fix → a test that
  fails before the fix and passes after. New document-conformant behaviour →
  tests for the normal path **and** the error/refusal path.
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
- Commits: Conventional Commits — `<type>(<scope>): …`, or `<type>: …` for
  repo-wide changes. The scope is the module/area touched, never the tool:
  `comms`, `obsw`, `bms`, `aocs`, `memory`, `fram`, `payloads`, `test`, `ci`,
  `docs`, `tasks`, `qual`. Examples taken from the history: `fix(comms): …`,
  `feat(bms): …`, `docs(aocs): …`, `test(comms): …`, `ci: …`.
- **PR body must contain**: what changed and why; the document reference (§1) it
  implements; the exact commands you ran and what they returned; anything you
  did **not** verify; and any open question or contradiction you hit.
- Stacked PRs are allowed and gated (CI has no base-branch filter).
- **Do not merge your own PR.** Merge happens on explicit human approval.
- Reviews: `REVIEWS.md` is the review contract — the in-house reviewer (subagent
  or human) applies it on every PR, and it is what we hand to an external
  reviewer. **CodeRabbit** does not auto-review this repo automatically (under 10
  stars): ask for it with an `@coderabbitai review` comment, one review per hour
  on the free tier.
- **Kilo Code Reviewer is filtered out.** Its runs end in `Review failed: The
  model output limit was reached` and publish no review, so its comments and its
  red check carry no information: ignore them — never read them as a verdict,
  never hold a merge for them, never open a PR "to fix" them, and when reporting
  the checks say it produced no text rather than calling it a pass or a failure.
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
