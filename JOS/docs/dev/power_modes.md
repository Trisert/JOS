# Power modes — MCU capability vs the OBSW requirement (T20)

Verification report for board row **T20 — "Power mode sleep/wakeup"**. It
answers one question: *does the STM32L496VGTx support the sleep/stop behaviour
the documentation set requires of the OBSW, and what does it take to use it?*
It is **docs-only**: no firmware change is proposed here (the toolchain needed
to build and prove one is not installed on the machine that produced this
report — see §7).

**Verdict: capability yes, implementation no.** The MCU offers Sleep, Stop,
Standby and Shutdown (`ELE_DREP`, MCU table). The specification asks the OBSW
for "low-power idle modes compatible with STM32L4 sleep states" (SPF V3
§3.6.3, repeated in `RED_SPF` and `SW_DREP` §3.1) and puts s3 READY in
"low-power mode". The firmware today runs the FreeRTOS idle task in Run mode:
the idle hook body is empty, tickless idle is disabled by default, and the
single `__WFI` in the project is an IRQ wait inside the SPI1 DMA transfer
loop — a wait primitive, not a system power mode. Closing the gap is a
design+firmware task with four open questions (wake source, timebase,
watchdog window, DMA quiescence), none of which the current documentation set
answers.

Line numbers for the SharePoint corpus refer to the extracted `.txt` files in
the local working copy (`sp_findings/spf_v3/`, produced by the 2026-09-12
document recovery — see row 42 of `TASKS.md`); that corpus is **not in this
repo**, so both document and section are named alongside the line.

## 1. (a) What the MCU supports, and where that is written

| Mode | Evidence |
|------|----------|
| Sleep | `ELE_DREP_Architecture_V01.docx.txt:296-297`; HAL `Drivers/STM32L4xx_HAL_Driver/Src/stm32l4xx_hal_pwr.c:444` (`HAL_PWR_EnterSLEEPMode`), doc comment `:421-441` |
| Stop 0 / Stop 1 / Stop 2 | `ELE_DREP…:296-297`; `stm32l4xx_hal_pwr.c:523` (`HAL_PWR_EnterSTOPMode`, dispatches to `stm32l4xx_hal_pwr_ex.c:1188/1239/1292`), doc comment `stm32l4xx_hal_pwr.c:493-517` |
| Standby | `ELE_DREP…:296-297`; `stm32l4xx_hal_pwr.c:556`, doc comment `:540-547` |
| Shutdown | `ELE_DREP…:296-297`; `stm32l4xx_hal_pwr_ex.c:1334` |
| Low-power Run / Low-power Sleep | **not listed** in the ELE table; present in the delivered HAL (`stm32l4xx_hal_pwr_ex.c:1129` `HAL_PWREx_EnableLowPowerRunMode`; Sleep notes `stm32l4xx_hal_pwr.c:424-439`) |

The ELE row is the *documented* capability set of the flight part
(`STM32L496VGT3 key features`, `ELE_DREP_Architecture_V01.docx.txt:273`;
LPTIM is listed among the timers at `:291`); the vendored HAL driver is the
set of modes this build can actually enter. The two agree on
Sleep/Stop/Standby/Shutdown.

Wake-up sources configured today: **EXTI0 only**, wired to the radio DIO1
(`Core/Src/stm32l4xx_it.c:291-293`). There is no RTC and no LPTIM peripheral
in the CubeMX configuration: `Core/Src/main.c:82-89` has no
`MX_RTC_Init`/`MX_LPTIM_Init`, `JOS.ioc:259-261` lists LPTIM only as RCC
frequency parameters, and the RTC is documented as **not clocked in this
build** (`HAL_RTC_MODULE_ENABLED` off, no backup-domain access —
`Core/Src/sram2_parity.c:131-133`). No periodic wake timer therefore exists
on the board as configured.

## 2. (b) What the documentation set requires of the OBSW

| Requirement | Source |
|-------------|--------|
| FreeRTOS "…low-power idle modes compatible with STM32L4 sleep states" | SPF V3 §3.6.3 "OBSW Design Description" (`SYS_SPF_V3Chapter3Paragraph3.6.docx.txt:45-46`); identical sentence in `RED_SPF_V3.docx.txt:2257` (Table 3.18) and `SW_DREP_Architecture_V01.docx.txt:136` (§3.1) |
| s3 READY — "Post-task idle; available for uplink; controllers in low-power mode" | `SYS_SPF…3.6.docx.txt:137`; `SW_DREP…:202`; `RED_SPF…:2348` |
| s3 READY — "Post-task idle state; low-power mode"; operational constraint "Low-power mode; payload execution not active" | `RED_SPF_V3.docx.txt:700` (Tab. 1.8) and `:708` |
| s4 ACTIVE — "controllers in medium-power mode" | `SYS_SPF…3.6.docx.txt:141`; `SW_DREP…:206`; `RED_SPF…:2352` |

**What the set does *not* specify** (checked, not assumed: a grep of the
recovered corpus for `wake`/`wakeup` returns only the s1 INIT rows, "Boot or
wake-up phase" — `RED_SPF_V3.docx.txt:630`, `SW_DREP…:194`, `SYS_SPF…:129`):

- which mode (Sleep vs Stop 0/1/2) the OBSW must use, in which state;
- wake sources, wake latency, sleep-window lengths or any duty-cycle target;
- the behaviour of the HAL 1 ms timebase (TIM6) or of the FreeRTOS tick
  across a sleep/stop;
- what the two watchdogs must do while the MCU sleeps;
- whether "controllers in low-power mode" means the OBC's own MCU, the
  payload/subsystem controllers, or both (both readings fit the text — **not
  resolved here**).

As written, the requirement is satisfiable by the *shallowest* Sleep
implementation; Stop/Standby are an engineering choice, not a spec mandate.

## 3. Reference correction: T20 cites SPF §3.6.4, which is "Memory Budget"

T20 reads "verificare supporto sleep mode compatibile con STM32L4 (SPF
§3.6.4)". In SPF V3, **§3.6.4 is "Memory Budget"**
(`SYS_SPF_V3Chapter3Paragraph3.6.docx.txt:230`). The power-mode requirement
lives in **§3.6.3 "OBSW Design Description"** (`:45-46`), echoed by `SW_DREP`
§3.1 and by the s1/s3/s4 rows of the state tables. T20's citation should be
read as §3.6.3; the row in `TASKS.md` is corrected in the same PR. (Same
class of finding as the day-1 "header PDT 32 B" withdrawal: the citation, not
the technical content, was wrong.)

## 4. (c) What JOS does today — code evidence

| Fact | Evidence |
|------|----------|
| FreeRTOS idle hook **enabled, body empty** → the idle task spins in Run mode | `JOS/Core/Inc/FreeRTOSConfig.h:77` (`configUSE_IDLE_HOOK 1`); `JOS/Core/Src/freertos.c:64-76` |
| Tickless idle **not configured**: `configUSE_TICKLESS_IDLE` absent from `FreeRTOSConfig.h` → kernel default `0` | `Middlewares/Third_Party/FreeRTOS/Source/include/FreeRTOS.h:761-762`; the tickless hook only compiles when enabled (`…/ARM_CM4F/port.c:508-510`, `__attribute__((weak))`) |
| No project code ever enters a power mode: **zero** `HAL_PWR_Enter*` / `HAL_PWREx_Enter*` calls in `App/` and `Core/Src/` | grep, see §7. PWR is used only for `HAL_PWREx_ControlVoltageScaling` (`Core/Src/main.c:321-324`), the PWR clock enable (`Core/Src/stm32l4xx_hal_msp.c:84`) and backup-domain unlock (`Core/Src/seu_mitigation.c:207-208`) |
| CPU pinned to the fastest legal point: VOS1 + MSI→PLL 80 MHz, Flash latency 4 | `Core/Src/main.c:314` (`SystemClock_Config`), `:321-324`, MSI range 6 / PLL ×40 ÷2 at `:332-337`, `:354` (`FLASH_LATENCY_4`) |
| The only sleep instruction in project code is `__WFI()` in the SPI1 DMA chunk wait (radio), woken by the DMA IRQ **or** the TIM6 tick, guarded by a baud-derived timeout | `App/comms/radiolib_hal.cpp:201`; rationale `:9-13` and `:194-196`; timeout `:184`, guard `:197-199`; DMA IRQs `Core/Src/stm32l4xx_it.c:247-255` |
| HAL timebase = TIM6 (1 ms); SysTick = FreeRTOS only | `Core/Src/stm32l4xx_hal_timebase_tim.c:5,79,102`; `Core/Src/stm32l4xx_it.c:198-218` and `:257-271`; `AGENTS.md` §4 |
| Two watchdogs run from sources that do not stop when the core sleeps: IWDG from LSI (~30.8 s guaranteed margin) and the external **STWD100** (`WD_IN` = PC15, requires an *edge* within tWD min 1.12 s) | `Core/Src/hw_watchdog.c:34-47` (timeout budget), `:113-127` (kick + STWD100), `Core/Inc/main.h:96-104` (JP1 strap, `EXT_WD_Pin`); refresh cadence 500 ms and single owner: `App/obsw/watchdog.c:412-423`; already noted in `docs/arch/README.md:80-81` |
| The shipped SPI path *assumes* the DMA keeps progressing while the core is in the WFI sleep | comment `App/comms/radiolib_hal.cpp:194-196`; this assumption is what a deeper mode must re-validate per mode |

So: sleep support in the MCU is present and unused; the one `__WFI` is an
in-task wait that enters Sleep as a side effect and is bounded by the 1 ms
TIM6 tick.

## 5. (d) Gap and what sleep/stop would require — with the unknowns

### Sleep path (the shallowest, spec-conformant option)

- Enter Sleep from the idle task: either `__WFI()` /
  `HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI)` in
  `vApplicationIdleHook`, or `configUSE_TICKLESS_IDLE 1` to let the kernel
  suppress the tick for the idle window.
- **Wake latency = the tick.** With both the 1 kHz SysTick (FreeRTOS) and the
  1 kHz TIM6 tick (HAL) enabled, a WFI sleep ends after ≤1 ms — the HAL says
  it explicitly: with WFI entry "tick interrupt have to be disabled if not
  desired as the interrupt wake up source" (`stm32l4xx_hal_pwr.c:440-441`).
  Plain WFI in the hook therefore buys tiny windows, not meaningful power
  reduction; suppressing the TIM6 tick interacts with every `HAL_GetTick()`
  timeout in the tree (SPI chunk timeouts, I2C/FRAM, Flash).
- **Tickless as shipped = still Sleep.** The vendored port's tickless path
  stops the SysTick (`…/ARM_CM4F/port.c:521-527`) and executes a plain `wfi`
  (`:583`); a plain WFI is Sleep because Stop requires setting `SLEEPDEEP`
  first — which the HAL does explicitly only for Stop
  (`stm32l4xx_hal_pwr_ex.c:1196-1197`) and clears for Sleep
  (`stm32l4xx_hal_pwr.c:473-474`). Deeper modes need a custom
  `vPortSuppressTicksAndSleep` and/or the `configPRE_SLEEP_PROCESSING` /
  `configPOST_SLEEP_PROCESSING` hooks (`port.c:573-586`).
- Idle-hook contract: the hook must return so the idle task can clean up
  deleted tasks (`Core/Src/freertos.c:66-73`); WFI returns on the next
  interrupt, so it is compatible, but the hook must not block.

### Stop path (only if the team wants real savings)

- **Wake source** — none exists today beyond EXTI0/radio DIO1. A periodic
  wake timer (RTC wakeup or LPTIM) must be added to the CubeMX config first,
  and any peripheral change must be reconciled with the OBC V2.0 netlist
  (`AGENTS.md` §4). Until then, a Stop entry can only be woken by radio DIO1.
- **Timebase** — Stop stops all VCORE clocks (HAL doc
  `stm32l4xx_hal_pwr.c:498-505`; Stop 0 `stm32l4xx_hal_pwr_ex.c:1164-1177`)
  and on exit the system clock comes back from HSI or MSI, selected by
  `STOPWUCK` (`stm32l4xx_hal_pwr.c:506-509`) — not the 80 MHz PLL
  configuration. `SystemClock_Config()` and Flash latency must be re-applied
  and the HAL tick restarted; every `HAL_GetTick()`-based timeout and the
  FreeRTOS tick accounting must be re-examined. **Not fully verifiable from
  this repo**: per-peripheral clock gating in Stop is RM0351 material (the
  repo vendors the HAL driver, not the reference manual).
- **Watchdogs set the ceiling** — IWDG keeps counting from LSI (no refresh →
  reset in ≤~30.8 s) and the external STWD100 needs a WDI *edge* within tWD
  min 1.12 s (`Core/Src/hw_watchdog.c:34-47,113-127`), while today the kick
  comes from a 500 ms monitor task that does not run during a sleep
  (`App/obsw/watchdog.c:412-423`). Any Stop design needs one of: sleep
  windows inside the tightest window; the kick parked on a wake timer; or the
  JP1 strap disabling the external WD (`Core/Inc/main.h:99-100`). What the
  watchdogs *should* do in low-power is **not specified** — SYS/ELE + HIL.
- **DMA / bus quiescence** — the radio path relies on DMA + IRQ while the
  core waits (`App/comms/radiolib_hal.cpp:9-13`), FRAM is on I2C1. In Stop
  the DMA clocks stop, so Stop may only be entered with no transfer in
  flight; no such gate exists today, and building one touches task/ISR
  interaction (out of scope for this docs-only PR).
- **Validation** — power draw and wake behaviour need hardware (HIL, issue
  #54). The Ceedling host suite covers arithmetic (e.g. timeout math), never
  MCU power states.

## 6. (e) Recommendation

1. **Close T20 as a verification** (this PR): capability present,
   implementation gap documented, section reference corrected to SPF §3.6.3.
2. **Next minimal implementable step** (new firmware work item, requires the
   ARM toolchain + HIL): Sleep in the idle path, with an explicit tick
   decision —
   (i) `WFI` in `vApplicationIdleHook`: cheap, keeps the timebase contract,
   windows ≤1 ms (low risk, small gain); or
   (ii) `configUSE_TICKLESS_IDLE 1` plus a defined wake timer: bigger gain,
   touches the timebase contract (`HAL_GetTick()` users and the TIM6 tick
   policy must be settled first).
3. **Do not start a Stop/Standby design** before SYS/ELE answer: (a) whether
   "low-power mode" in the s3 row binds the OBC MCU at all; (b) the watchdog
   policy in low power; (c) the periodic wake source. These are blockers for
   design, not code questions.
4. Acceptance evidence for any power work must be measured current on
   hardware; no host test can prove a power mode.

## 7. What was not run

Docs-only PR; the machine that produced it has **no firmware toolchain** — no
`arm-none-eabi-gcc`, no pinned `cppcheck` 2.13.0, no `ruby`/Ceedling, no
`nix` (only host `make`/`gcc`). Per `AGENTS.md` §2 these gates are therefore
**not run**, and no gate is claimed green:

- `make release`, `make crc-stamp`, `arm-none-eabi-size` — **not run** (no
  `arm-none-eabi-gcc`).
- `make test` — **not run** (no Ceedling/Unity toolchain).
- `make coverage` — **not run** (same).
- `make cppcheck` (2.13.0 pinned) — **not run** (not installed).
- HIL/bench power measurement — **not run** (no hardware here).

No C source is touched. CI (firmware build, Ceedling, static-analysis, CodeQL,
actionlint) is the authoritative gate for the head commit; it is read on the
PR, not predicted here.

## 8. Sources

- SPF V3 §3.6 extract: `SYS_SPF_V3Chapter3Paragraph3.6.docx.txt` (lines cited
  above), recovered copy under `sp_findings/spf_v3/` — **outside this repo**.
- `RED_SPF_V3.docx.txt`, `SW_DREP_Architecture_V01.docx.txt`,
  `ELE_DREP_Architecture_V01.docx.txt` — same location.
- Repo evidence: paths cited inline; the two `grep` commands used for the
  negative claims ("no `HAL_PWR_Enter*` call", "no `configUSE_TICKLESS_IDLE`")
  are quoted in the PR body.
