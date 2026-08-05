# OBSW Core API — State Machine, Watchdog, LastStates

## State Machine (`App/obsw/state_machine.c`)

Five-state FSM (see `docs/arch/README.md` §Operational State Machine).

| Function | Purpose |
|----------|---------|
| `try_transition(state_t next)` | Attempt a transition; returns `int` (0 = ok). Propagates LastStates write errors. |
| `state_machine_task()` | FreeRTOS task @ 10 Hz; runs transitions, kicks watchdog. |

Transitions are logged to the LastStates pool via `memory_laststates_write()`.

## Watchdog (`App/obsw/watchdog.c`)

Dedicated task monitoring all other tasks via FreeRTOS tick counters.
Any task deviating from its nominal tick profile is flagged anomalous.

- Hardware IWDG (~32 s) kicked in the main OBSW task loop.
- Every task must register with the watchdog on init.

## LastStates Pool (`App/memory/memory.c`)

Flash-backed ring buffer of state transitions — primary forensic tool.

| Property | Value |
|----------|-------|
| Location | Internal Flash, `0x08080000` |
| Entry size | 128 B |
| Max entries | 64 (circular) |
| Total | 8 KB |
| Linker region | `LASTSTATES` in `STM32L496VGTX_FLASH.ld` |

| Function | Purpose |
|----------|---------|
| `laststates_write(state_entry_t *entry)` | Write one entry; erases/reclaims pool pages on wrap (STM32L4 **pages**, not sectors). Returns `int`. |
| `laststates_log(...)` | Hook called on every transition; returns `int` so errors propagate to `try_transition`. |
| `laststates_dump_all(uint8_t *out, size_t *len)` | Serialise pool for downlink (`SEND_DATA` with LastStates alias). |

> Implemented (PR #1). Previously a stub.
