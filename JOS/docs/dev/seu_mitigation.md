# SEU mitigation — periodic RAM scrubbing (W2-5)

Single-Event Upsets (SEU) are bit flips caused by charged particles. In LEO
they are the dominant soft-failure mode for an SRAM-based OBC. RedPill answers
them in two layers:

| Layer | Where | What it does |
|-------|-------|--------------|
| W2-3 `sram2_parity` | hardware | SRAM2 parity bit per byte; a corrupted **read** raises an NMI, the handler records the context in LastStates and resets |
| **W2-5 `seu_mitigation`** | software | reads the critical structures on a timer, **corrects** them from a redundant copy, and counts SEU events across the parity reset |

Parity alone is not enough: it only fires when the corrupted byte is read, and
it cannot repair anything — the answer is always a reboot. Scrubbing closes
both gaps.

Standards: **NASA-STD-8739.8** (fault tolerance, data integrity, no silent
failure), **ECSS-E-ST-40C** (recorded failure context), **ECSS-Q-ST-80C Rev.2**
(FDIR), **NASA Power of Ten** #2 (bounded loops) and #3 (no allocation after
init).

## 1. Redundancy layout

For every region scrubbed with the *golden* policy there are three copies of
the truth, in three different places, so a single upset can never outvote the
other two:

```
live object        SRAM2 (.sram2) or SRAM1, owned by the application
shadow copy        SRAM2 shadow pool (.sram2_noinit), a different address
reference CRC-32   SRAM1, inside the module's region table
```

The shadow pool is `NOLOAD`: the SRAM2 hardware erase performed by
`sram2_parity_init()` zeroes it with valid parity, so it costs no Flash.

## 2. Registered regions

| id | region | memory | policy |
|----|--------|--------|--------|
| 0 | `obsw_state` (state machine critical struct) | SRAM2 `.sram2` | golden |
| 1 | LastStates pool bookkeeping (`laststates_mirror_t`) | SRAM1 | golden |
| 2 | beacon staging buffer | SRAM2 `.sram2_noinit` | touch |
| 3 | uplink decode buffer | SRAM2 `.sram2_noinit` | touch |
| 4 | downlink chunk buffer | SRAM2 `.sram2_noinit` | touch |

The LastStates bookkeeping deliberately stays in SRAM1: the parity NMI handler
writes a LastStates entry, and the block that just reported a parity error
must not sit on the path that records its own failure.

Comms buffers change with every beacon and every uplink, so there is no golden
content to vote on. They are *touched* instead — every byte is read, which is
what makes the parity hardware surface a dormant upset now, at a harmless
moment, rather than in the middle of a pass.

## 3. The vote

Each pass takes the three legs in one locked instant (so a concurrent commit
cannot look like a double corruption), then decides with interrupts enabled:

| live vs CRC | shadow vs CRC | conclusion | action |
|-------------|---------------|------------|--------|
| match | match | healthy | — |
| match | differ | the shadow was hit | shadow rebuilt from live |
| differ | match | the live object was hit | **live rewritten from the shadow** |
| differ | differ, live == shadow | the CRC word was hit | reference CRC recomputed |
| differ | differ, live != shadow | unrecoverable | `.sram2` objects restored from the Flash load image via `sram2_restore_from_image()`; otherwise recorded and left alone |

Before any rewrite the module re-checks, under the lock, that the live object
still matches the snapshot it voted on. A legitimate update that raced the
pass is therefore never "repaired" away — it is simply re-examined next time.

Every non-healthy outcome is written to the LastStates pool with trigger
`TRIGGER_SEU_SCRUB` (11) and a `seu_record_t` blob (magic `"SEUR"`) carrying
the region, the number of flipped bytes/bits, the offset of the first bad byte
and the cumulative counters — enough for ground to trend the radiation
environment.

## 4. Ownership contract

The scrubber assumes a golden region only changes when its owner says so.
Every legitimate write must therefore be followed by
`seu_mitigation_commit()`, **atomically** with the write itself:

```c
seu_mitigation_lock();                              /* PRIMASK, NMI-safe */
obsw_state.current_state = target;
(void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
seu_mitigation_unlock();
```

Call sites today: `state_machine.c` (transition, beacon override, BMS stub)
and `memory.c` (`laststates_write()` index advance). The lock is PRIMASK based
rather than `taskENTER_CRITICAL()` because `laststates_write()` is also called
from the parity NMI, where the FreeRTOS critical-section assertion would spin
forever.

## 5. Counting SEU events across a reset

A parity NMI resets the MCU, so an in-RAM counter cannot survive its own
event. `seu_mitigation_nmi_hook()` runs from `NMI_Handler()` **before**
`sram2_parity_nmi_handler()` and increments a saturating counter in RTC backup
register 31 (backup domain, survives a system reset); register 30 holds the
value already reported. At the next boot `seu_mitigation_init()` notices the
difference, writes one `SEU_EVENT_PARITY_HISTORY` record and acknowledges it,
so the pool gets one entry per event rather than one per boot.

The counter is cleared by a backup-domain reset or by a loss of VBAT — it is
mission-cumulative, not lifetime-cumulative. Set `SEU_USE_BACKUP_REGISTER=0`
to drop the dependency (the counter then lives in SRAM1 and resets with the
MCU).

## 6. Task and timing

`seu_scrub_task_create()` starts `seuScrub` at `osPriorityLow` with a 1536 B
stack. It sleeps `SEU_SCRUB_INTERVAL_MS` (default **5 minutes**) between
passes. One pass is a few hundred bytes of `memcpy` plus a bitwise CRC-32 —
microseconds of work, of which only the snapshot (a `memcpy` of at most 256
bytes) is done with interrupts masked. The STM32 CRC peripheral is left free
for the boot-image check (W3-1).

Build-time knobs (`seu_mitigation.h`): `SEU_SCRUB_INTERVAL_MS`,
`SEU_MAX_REGIONS`, `SEU_SHADOW_POOL_BYTES`, `SEU_MAX_REGION_BYTES`,
`SEU_USE_BACKUP_REGISTER`, `SEU_BKP_COUNT_INDEX`, `SEU_BKP_ACK_INDEX`,
`SEU_SCRUB_TASK_STACK`.

## 7. API

```c
void         seu_mitigation_init(void);            /* snapshot + arm         */
osThreadId_t seu_scrub_task_create(void);          /* periodic scrub task    */
int          seu_mitigation_register_region(seu_region_id_t id, void *addr,
                                            size_t len, seu_policy_t policy);
int          seu_mitigation_commit(seu_region_id_t id);   /* after a write   */
int          seu_mitigation_scrub_once(void);      /* forced pass (TC/test)  */
void         seu_mitigation_get_stats(seu_stats_t *out);  /* telemetry       */
uint32_t     seu_mitigation_event_count(void);
void         seu_mitigation_lock(void);            /* PRIMASK, NMI-safe      */
void         seu_mitigation_unlock(void);
void         seu_mitigation_nmi_hook(void);        /* from NMI_Handler()     */
```

## 8. Init order (main.c)

```
sram2_parity_init();      /* erases SRAM2, restores the .sram2 image  (W2-3) */
...
laststates_init();
lora_init();
state_machine_init();
watchdog_monitor_init();
seu_mitigation_init();    /* snapshots what the calls above produced  (W2-5) */
...
seu_scrub_task_create();
```

`seu_mitigation_init()` must run after the owners are initialised, otherwise
the shadows would capture pre-init content. Commits issued before it are
no-ops by design.

(T1.6) Before T1.6 this init order also had to coordinate with a parallel
`App/obsw/scrub.c` FRAM-golden restore (`scrub_init()`); that module was
removed in T1.6 as redundant and the load-bearing ordering comment in
`main.c` was deleted with it.

## 9. Verification status

- `make all` builds clean with `arm-none-eabi-gcc` (no new warnings);
  `.sram2` = 24 B, `.sram2_noinit` = 768 B (512 B shadow pool + 256 B of comms
  buffers), all inside the 64 KB `RAM2` region.
- Fault-injection on hardware (flip a bit through the debugger, confirm the
  repair and the LastStates record) is **pending** — it needs the flight board
  and is tracked with the rest of the W2 hardening verification.

## 10. Known limitations

- Scrubbing protects the *registered* structures, not all of RAM. Task stacks
  and the FreeRTOS heap are covered only by SRAM2 parity where they live in
  SRAM2, which today they do not.
- Parity detects but never corrects; the correction here comes from the
  redundant copy, so an upset that hits both copies between two passes is
  logged as unrecoverable.
- A missing `seu_mitigation_commit()` at a new write site would make the
  scrubber revert that write. New mutators of a golden region must follow the
  contract in §4.
