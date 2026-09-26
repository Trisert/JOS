# OBSW Core API — State Machine, Watchdog, LastStates

## State Machine (`App/obsw/state_machine.c`)

Five-state FSM (see `docs/arch/README.md` §Operational State Machine).

| Function | Purpose |
|----------|---------|
| `state_machine_request_transition(target, trigger)` | Request a transition through the gates (boot-CRC / SRAM2-parity confinement, SoC gates). Returns 0 when the OBSW is in `target` afterwards (committed, or already in CRIT), -1 when a gate refused it. |
| `state_machine_unrecorded_transitions()` | Transitions committed although their LastStates record could not be written (Flash failure). |
| `state_machine_task()` | FreeRTOS task @ 10 Hz: boot sequence, 1 Hz EPS poll, autonomous battery check, watchdog kick. |

Rules (file-static `try_transition()` / `enter_safe_state()`):

- Every committed transition is logged to LastStates **before** the commit.
  A failed record does **not** veto the transition (it used to, which froze the
  OBSW on a sick Flash page — boot stuck in OFF, no CRIT on low battery); the
  loss is counted instead.
- CRIT → CRIT is a no-op with no record (the 10 Hz battery check used to log
  it every 100 ms).
- Every boot starts from s0 with an **unknown** battery: the task resets
  `current_state` and the BMS snapshot restored from the FRAM golden copy
  before the boot sequence. Only the ground-commanded beacon override survives
  a reset.
- The EPS is polled on the first loop iteration, then every 10th (1 Hz).

## Watchdog (`App/obsw/watchdog.c`)

Dedicated task monitoring all other tasks via FreeRTOS tick counters.
A task silent for more than 3x its declared period is escalated:

- a `TRIGGER_WATCHDOG` LastStates record, `dual_bank_mark_boot_fault()` (a
  task hanging on every boot arms the golden-image fallback) and a **reset**
  of the OBC. (It used to be suspended, with the IWDG still kicked: no
  recovery path at all — a suspended loraRX meant a deaf satellite.)
- if the stalled task holds the LastStates pool mutex, the record would wedge
  the monitor: the escalation is deferred and retried, at most
  `WDG_MAX_HOLDER_DEFERRALS` (60 scans, 30 s) in a row, then the OBC resets
  without the record. The count covers one stall episode: a scan with no
  stalled task restarts it, so a task that recovers does not carry old
  deferrals into its next, unrelated stall.
- The IWDG (~31 s) and the STWD100 are refreshed by the monitor task every
  500 ms, unconditionally. The IWDG is reloaded whenever it has been started,
  even if its prescaler/reload did not settle. Long pre-scheduler work calls
  `hw_watchdog_boot_kick()`.
- Every task must register with the watchdog on init.

### External watchdog (OBC V2.0 STWD100YNYWY3F)

WDI is driven by PC15 (net `WD_IN`, push-pull output in `MX_GPIO_Init`),
WDO feeds NRST: if WDI stops toggling within tWD the board resets.
`hw_watchdog_kick()` toggles PC15 on every kick alongside the IWDG reload,
and the monitor task calls it every 500 ms (`WDG_MONITOR_PERIOD_MS`).

Vincolo: il periodo di kick (500 ms) deve restare < tWD minimo del variant Y
(datasheet STWD100 DocID14134 Rev 11, Tab. 4: tWD = 1,12 s min / 1,6 s tip /
2,24 s max) — margine ~2x sul caso peggiore.

## LastStates Pool (`App/memory/memory.c`)

Flash-backed ring buffer of state transitions — primary forensic tool.

| Property | Value |
|----------|-------|
| Location | Internal Flash, `0x08080000` |
| Entry size | 128 B |
| Max entries | 64 slots (circular); 48–63 records once wrapped (erase-ahead, see `docs/api/memory.md`) |
| Total | 8 KB |
| Linker region | `LASTSTATES` in `STM32L496VGTX_FLASH.ld` |

| Function | Purpose |
|----------|---------|
| `laststates_write(const laststates_entry_t *entry)` | Write one entry; recycles the oldest 2 KB page ahead of the cursor (STM32L4 **pages**, not sectors). Returns `int`. |
| `laststates_log(...)` | File-static hook in `state_machine.c`, called on every transition. |
| `laststates_dump_all(uint8_t *out, size_t *len)` | Serialise the pool oldest-first for downlink (no downlink path is wired yet: `SEND_DATA` is refused as unimplemented). |

> Implemented (PR #1). Previously a stub.
