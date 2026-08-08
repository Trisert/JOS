# SRAM2 parity protection for critical data (W2-3)

Hardening card **W2-3**. Standards: **NASA-STD-8739.8** (data integrity, fault
containment, no silent failure), **ECSS-E-ST-40C** (recorded failure context),
**ECSS-Q-ST-80C** (fault detection).

## Why

The STM32L496VGTx has two RAM blocks:

| Block | Address      | Size   | Error detection |
|-------|--------------|--------|-----------------|
| SRAM1 | `0x20000000` | 256 KB | none            |
| SRAM2 | `0x10000000` | 64 KB  | **byte parity** |

A single-event upset in SRAM1 is undetectable: the OBSW would keep running on a
flipped bit. In SRAM2 the same flip is detected on read, raises a non-maskable
interrupt and is contained.

## What is protected

| Data | Section | Where |
|------|---------|-------|
| `obsw_state` — operational state, beacon override, BMS snapshot | `.sram2` | `App/obsw/state_machine.c` |
| `comms_beacon_buf` (128 B), `comms_rx_buf` / `comms_tx_buf` (64 B) | `.sram2_noinit` | `App/comms/comms.c` |

Placement macros come from `Core/Inc/sram2_parity.h`:

- `SRAM2_CRITICAL` → `.sram2`, an **initialised** section whose load image the
  linker keeps in Flash; `sram2_parity_init()` copies it into SRAM2, so static
  initialisers behave exactly as for `.data`.
- `SRAM2_CRITICAL_NOINIT` → `.sram2_noinit`, a `NOLOAD` section zeroed by the
  SRAM2 hardware erase at boot; costs no Flash.

Both sections live in the `RAM2` region of `STM32L496VGTX_FLASH.ld`, with a
link-time `ASSERT` against overflowing the 64 KB block.

## Boot sequence

`main()` calls `sram2_parity_init()` first (USER CODE BEGIN Init), before any
other init, because it wipes the whole block:

1. enable the SYSCFG clock (parity flag + SRAM2 erase control live there);
2. if `SYSCFG_CFGR2.SPF` is already latched, **build** a
   `SRAM2_EVENT_BOOT_LATCH` entry (all status registers are sampled here) and
   clear the flag (a stale flag would mask the next event);
3. hardware-erase SRAM2 (`SYSCFG_SCSR.SRAM2ER`) — this writes valid parity for
   every byte, so no later read can trip a spurious NMI. The erase is started
   after the `SYSCFG_SKR` unlock sequence (`0xCA`, `0x53`) and the write
   protection of `SRAM2ER` is **re-armed straight after** by writing a
   non-key value, so no stray write can wipe the block at runtime. Completion
   is detected on `SRAM2ER` clearing (not on `SRAM2BSY` alone, which can still
   read 0 while the APB write that starts the erase is posted);
4. copy the `.sram2` load image out of Flash — **skipped** when the erase did
   not complete, so a half-erased block is never masked by fresh data; a
   `SRAM2_EVENT_ERASE_FAIL` entry is built instead;
5. read back `FLASH_OPTR.SRAM2_PE` and expose the result via
   `sram2_parity_is_enabled()`.

### Deferred persistence of the boot findings

`sram2_parity_init()` must run before `laststates_init()`, which resets the
LastStates pool write index. A record written from `init()` would therefore sit
at an index the first post-boot transition overwrites (and on a pool that
already holds an entry at index 0 the double-word program would fail with
`PGSERR`). The entries are consequently only *built* at detection time and
written by `sram2_parity_persist_boot_records()`, which `main()` calls
immediately after `laststates_init()`. The write is one shot: a wedged Flash
cannot turn boot into a write storm.

### Safe state after a boot finding

`sram2_parity_boot_fault()` reports a latched parity flag or a failed erase.
The state-machine task consults it after `STATE_INIT` and transitions to
`STATE_CRIT` (safe mode, beacon only) with trigger `TRIGGER_SRAM2_PARITY`
instead of continuing to `STATE_READY`. The latch blocks the autonomous
battery recovery out of `STATE_CRIT`; only a ground-commanded transition
(`TRIGGER_GROUND_CMD`) clears it. The boot fault flag stays set even when the
Flash record could not be written — the safe-state decision never depends on
the evidence reaching Flash.

## Enabling the hardware check

The parity check is **not** a register bit: it is the user option byte
`FLASH_OPTR.SRAM2_PE`, active low (`0` = enabled, reset value `1` = disabled).
Two ways to set it:

- **Flashing procedure (default, preferred for flight):** program `SRAM2_PE = 0`
  and `SRAM2_RST = 0` with the programmer, e.g.
  `STM32_Programmer_CLI -c port=SWD -ob SRAM2_PE=0 SRAM2_RST=0`.
- **Firmware, opt-in:** build with `-DSRAM2_PARITY_PROGRAM_OPTION_BYTE=1`.
  `sram2_parity_init()` then programs the option bytes once and launches them,
  which resets the MCU. Off by default: option bytes are non-volatile and the
  launch reboots the board.

Optional: `-DSRAM2_PARITY_BREAK_TIM_LOCK=1` also routes and locks the parity
error onto the TIM1/8/15/16/17 break inputs so PWM actuators are forced to a
safe state in hardware (lock released only by a system reset).

## Fault path

`NMI_Handler()` (`Core/Src/stm32l4xx_it.c`) calls `sram2_parity_nmi_handler()`,
which:

1. classifies the NMI (`SRAM2_EVENT_PARITY_NMI` when `SPF` is set, otherwise
   `SRAM2_EVENT_OTHER_NMI` — the RCC clock security system also vectors here);
2. fills a `sram2_parity_record_t` (SYSCFG `CFGR2`/`SCSR`, `FLASH_OPTR`,
   `RCC_CIFR`, `SCB_ICSR`, region bounds, event counter) **on the handler
   stack in SRAM1** — memory that just reported a parity error is not trusted
   to carry its own failure report;
3. clears the source flags, persists the record in the LastStates pool with
   trigger `TRIGGER_SRAM2_PARITY`, then `NVIC_SystemReset()`.

Before the record is written the handler forces the actuators to their safe
state at register level (`TIM1` `MOE` and `CEN` cleared, no HAL handle, no
mutex), so nothing stays driven while the reset propagates.

The old CubeMX behaviour (`while (1)`) is replaced: no silent hang.

Known gap (tracked, must close before flight): the Flash write inside the NMI
uses the HAL, whose `HAL_GetTick()` timeout cannot expire in NMI context, and
the independent watchdog (IWDG) is not initialised in this build yet — a Flash
controller that never clears `BSY` would hang the handler.

## Recovery helper

`sram2_restore_from_image(obj, len)` copies any `.sram2` object back from its
immutable Flash load image. `state_machine_init()` uses it when the
`obsw_state.magic` check fails, and it is the building block for the periodic
scrubbing planned in **W2-5** (SEU mitigation).

## Verification

- `make all` with arm-none-eabi-gcc 15.2 (NixOS build container): green.
- Symbol placement in `build/JOS.elf`:
  `obsw_state @ 0x10000000`, `comms_beacon_buf @ 0x10000018`,
  `comms_rx_buf @ 0x10000098`, `comms_tx_buf @ 0x100000d8`;
  `.sram2` = 24 B, `.sram2_noinit` = 256 B, region `0x10000000..0x10010000`.
