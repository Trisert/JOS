#include "state_machine.h"
#include "bms.h"           /* bms_poll(): EPS link + SoC threshold logic */
#include "memory.h"        /* laststates_write(): LastStates pool API */
#include "watchdog.h"
#include "boot_crc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "memory.h"         /* laststates_write() prototype */
#include "sram2_parity.h"   /* SRAM2_CRITICAL placement (W2-3) */
#include "seu_mitigation.h" /* redundant snapshot + scrubbing (W2-5) */
#include <string.h>

/* ---------- Private variables ---------- */
static osMutexId_t state_mutex;
static osThreadId_t sm_task_handle;

/* Latched when the OBSW is confined to the safe state because of an SRAM2
   parity finding at boot.

   Enforced centrally in try_transition() (Kilo #23, comment id 3740885216),
   next to the boot-CRC confinement, so EVERY caller is covered: the previous
   version consulted it only from the battery-recovery branch while the comms
   path walked straight into try_transition() with STATE_READY / STATE_ACTIVE,
   both legal from CRIT. Two safety latches, only one of which confined
   anything.

   Lifetime: the finding itself lives in a reset-stable .noinit store, and
   sram2_parity_init() no longer consumes it - it is released by
   sram2_parity_boot_fault_ack(), which the boot sequence below calls only
   after the OBSW has actually reached STATE_CRIT. So a warm reset before that
   point (watchdog, boot-CRC policy, COMMS_TC_RESET, dual-bank reboot)
   re-enters the safe state instead of silently clearing it. This .bss copy is
   the in-run half and lasts for this boot; a battery recovery does not clear
   a memory-integrity finding. */
static uint8_t parity_safe_latched = 0U;

/* ---------- Critical OBSW state (SRAM2, hardware parity) ----------
   Operational state, beacon override and the last BMS snapshot decide every
   autonomous action of the spacecraft, so they live in the parity-protected
   SRAM2 block: a bit flip here raises an NMI and is contained by
   sram2_parity_nmi_handler() instead of silently steering the mission.
   The .sram2 image is copied from Flash by sram2_parity_init(), which MUST
   run before state_machine_init(); static initialisers below are therefore
   effective exactly as for ordinary .data. */
#define OBSW_STATE_MAGIC   0x4F53574DU   /* "OSWM" */

typedef struct {
    uint32_t     magic;                    /* integrity marker for ground     */
    obw_state_t  current_state;            /* current operational state       */
    uint32_t     beacon_interval_override; /* 0 = per-state default           */
    bms_status_t bms;                      /* latest battery snapshot (stub)  */
} obsw_critical_state_t;

static SRAM2_CRITICAL obsw_critical_state_t obsw_state = {
    .magic                    = OBSW_STATE_MAGIC,
    .current_state            = STATE_OFF,
    .beacon_interval_override = 0U,
    /* Stub BMS - per RED_DES_ElectronicArchitecture_V1:
         BQ76905 on EPS board via local I2C to EPS MCU,
         OBC queries EPS over subsystem SPI.
       Default voltage for 2S Li-ion: 7400 mV nominal.

       soc = 0 with valid = false is the FAIL-SAFE boot state, not a
       measurement: no EPS telemetry has been read yet, so the SoC is
       UNKNOWN and every SoC-gated path below stays closed. The previous
       default was soc = 100, which was indistinguishable from real
       telemetry and made a dead EPS link look like a full battery
       (fix/bms-soc-gating). bms_poll() promotes this to a real reading
       once the EPS frame format exists; until then the OBSW correctly
       reports "unknown" instead of "full". */
    .bms                      = {
        .soc        = 0,
        .temp_c     = 250,   /* 25.0 C */
        .voltage_mv = 7400,
        .valid      = false,
    },
};

static const osThreadAttr_t sm_task_attrs = {
    .name       = "stateMachine",
    .stack_size = 256 * 4,
    .priority   = osPriorityAboveNormal,
};

/* Default battery thresholds */
static const bms_thresholds_t default_thresholds = {
    .b_opok   = 80,
    .b_commok = 60,
    .b_crit   = 40,
    .b_scrit  = 25,
};

/* Read the OBSW's cached battery snapshot.
 *
 * Deliberately NOT bms_get_status(): that name belongs to App/bms/bms.c,
 * which owns the EPS link and its own cache. The two used to coexist as two
 * same-named functions with different backing storage and no relationship
 * between them, so the gates here compared against a constant that the EPS
 * side never touched. This module reads the SRAM2 snapshot, and
 * bms_refresh_snapshot() is what keeps that snapshot fed from the EPS. */
static bms_status_t obsw_bms_snapshot(void)
{
    return obsw_state.bms;
}

/* For testing: allow overriding SoC from outside.
 *
 * This stands in for a completed EPS poll, so it also marks the snapshot
 * VALID: the tests are exercising the gates with a battery reading the OBSW
 * would actually have received. The un-stubbed default stays invalid, which
 * is what test_..._unknown_soc_* asserts. */
void bms_set_soc_stub(uint8_t soc)
{
    /* Update and re-snapshot atomically: the scrub task must never observe
       the struct between the write and the commit, or it would "repair" a
       legitimate change back to the previous value (W2-5). */
    seu_mitigation_lock();
    obsw_state.bms.soc   = soc;
    obsw_state.bms.valid = true;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    /* Write-through to the FRAM golden copy (W2-5): the SRAM2 shadow alone
     * does not survive a reboot, so without this the next boot would
     * re-snapshot the post-reset RAM instead of the truth committed here.
     * Outside the SEU lock: blocking I2C, task context only. */
    (void)seu_mitigation_sync(SEU_REGION_OBSW_STATE);
}

void *state_machine_critical_region(size_t *len)
{
    if (len != NULL) { *len = sizeof(obsw_state); }
    return &obsw_state;
}

/* Companion to bms_set_soc_stub(): mark the CURRENT snapshot as
 * un-backed-by-telemetry, leaving its soc byte where it is.
 *
 * The byte is deliberately kept, not zeroed: the hazard this models is a
 * STALE reading that still says "full" while nothing has read the battery.
 * A test that zeroed the SoC instead would pass on the threshold alone and
 * prove nothing about the valid flag. */
void bms_clear_soc_stub(void)
{
    seu_mitigation_lock();
    obsw_state.bms.valid = false;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    (void)seu_mitigation_sync(SEU_REGION_OBSW_STATE);
}

/* ---------- LastStates logging ---------- */
static int laststates_log(uint8_t from, uint8_t to, uint8_t trigger,
                           const uint8_t *ctx, size_t ctx_len)
{
    laststates_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = (uint32_t)osKernelGetTickCount();
    entry.state_from = from;
    entry.state_to   = to;
    entry.trigger    = trigger;
    if (ctx && ctx_len > 0) {
        size_t n = (ctx_len < sizeof(entry.context)) ? ctx_len : sizeof(entry.context);
        memcpy(entry.context, ctx, n);
    }
    int rc = laststates_write(&entry);
    if (rc != 0) {
        /* Flash write failed: the transition cannot be considered safely
           persisted. Signal via the QM fault path (no silent success). */
        return -1;
    }
    return 0;
}

/* ---------- State transition logic ---------- */
static int try_transition(obw_state_t target, uint8_t trigger)
{
    bms_status_t bms;
    int ok = 0;

    /* Boot-CRC safe state (docs/dev/hardening.md 2.4): while the running
       image cannot be trusted the OBC is confined to STATE_CRIT — beacon
       only, payloads and nominal ops inhibited — so ground can diagnose and
       re-upload. Only CRIT (and the INIT boot bookkeeping) stay reachable. */
    if ((boot_crc_image_trusted() == 0) &&
        (target != STATE_CRIT) && (target != STATE_INIT)) {
        return -1;
    }

    /* SRAM2 parity confinement, enforced in the same central place and for
       the same reason (Kilo #23, comment id 3740885216): a memory-integrity
       finding is not something a battery recovery or a ground command may
       walk out of. Only CRIT (and the INIT boot bookkeeping) stay reachable
       while the latch is held; see the declaration for its lifetime. */
    if ((parity_safe_latched != 0U) &&
        (target != STATE_CRIT) && (target != STATE_INIT)) {
        return -1;
    }

    switch (target) {
    case STATE_INIT:
        /* Only valid from OFF (boot) */
        ok = (obsw_state.current_state == STATE_OFF);
        break;

    case STATE_CRIT:
        /* Any state can enter CRIT on low battery / critical event */
        ok = 1;
        break;

    case STATE_READY:
        /* s1→s3: after boot + antenna deploy success */
        /* s2→s3: battery recovers to b_opok */
        if (obsw_state.current_state == STATE_INIT) {
            ok = 1;  /* assume antenna deploy + self-test passed */
        } else if (obsw_state.current_state == STATE_CRIT) {
            bms = obsw_bms_snapshot();
            /* SoC-gated recovery (SPF r.613/r.816): requires a VALID
               reading at or above B_OPOK. An unknown SoC does not
               satisfy it, so the OBSW stays in CRIT rather than
               recovering on a battery level nobody measured. */
            ok = bms_soc_allows_payload(&bms, &default_thresholds);
        }
        break;

    case STATE_ACTIVE:
        /* s3→s4: scheduled task / ground command */
        /* s2→s4: ground command + stable battery */
        if (obsw_state.current_state == STATE_READY) {
            ok = 1;
        } else if (obsw_state.current_state == STATE_CRIT) {
            bms = obsw_bms_snapshot();
            /* Manual payload activation requires SoC >= B_OPOK
               (SPF r.824) — valid data again, fail closed. */
            ok = bms_soc_allows_payload(&bms, &default_thresholds) &&
                 (trigger == TRIGGER_GROUND_CMD);
        }
        break;

    case STATE_OFF:
        /* Cannot transition to OFF in flight */
        ok = 0;
        break;
    }

    if (!ok) {
        return -1;
    }

    if (laststates_log((uint8_t)obsw_state.current_state, (uint8_t)target, trigger, NULL, 0) != 0) {
        /* LastStates persistence failed (Flash write/erase error). The
           transition still proceeds, but we flag it so the QM fault path
           can record the anomaly instead of silently reporting success. */
        return -1;
    }

    /* TEMPORAL BOUND of the Flash write above, which runs while holding
       state_mutex (documented here instead of moved out: the log-then-commit
       order is load-bearing - a record must never describe a transition that
       did not commit, and a commit must never land without its record - so
       the write cannot leave this critical section without changing the
       failure semantics every caller above relies on).
       Worst-case hold, all terms bounded:
         - own write (laststates_write): Flash reads for the slot scan, one
           optional page erase (cycle-bounded, FLASH_PAGE_TIMEOUT_CYCLES =
           0x400000 ~= 52 ms @ 80 MHz) and 16 double-words x cycle-bounded
           dword wait (FLASH_DWORD_TIMEOUT_CYCLES = 0x20000 ~= 1.6 ms):
           ~= 80 ms worst case. No blocking RTOS call under the pool lock.
         - pool-lock wait (osWaitForever): at most one in-progress writer.
           laststates_write() holds it across the same bounded sequence as
           above; dual_bank.c:ls_append() across 16x HAL_FLASH_Program
           (HAL tick timeout, nominal < 1 ms). The wait terminates because
           every holder runs to release: in particular the watchdog monitor
           NEVER suspends the pool-mutex holder (watchdog_suspend_allowed()),
           which is what used to turn this wait into a wedge.
         - lock ordering is state_mutex -> ls_pool_mutex everywhere and never
           the reverse (no pool-locked path takes state_mutex), and state_mutex
           carries osMutexPrioInherit, so priority inversion across this
           section is bounded by the hold above, not by a medium-priority
           task.
       Net: nominal ~= 160 ms, pathological (wedged Flash controller hitting
       a HAL tick timeout) up to tens of seconds, always terminating. Readers
       blocked on state_mutex for that long (beacon cadence, comms ground
       commands) miss a cycle; the IWDG backstop and the watchdog monitor
       never take state_mutex and are unaffected. */

    seu_mitigation_lock();
    obsw_state.current_state = target;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();

    /* NOTE: no seu_mitigation_sync() here. The write-through to the FRAM
     * golden copy (W2-5) runs a blocking I2C transaction (up to 1 s
     * timeout) and every caller of try_transition() holds state_mutex, so
     * syncing here would wedge the whole state-transition critical section
     * on a sick bus. The commit above already moved the shadow/CRC pair, so
     * the scrubber sees a consistent image; each caller performs the
     * best-effort FRAM sync AFTER releasing state_mutex. A FRAM failure is
     * counted in seu_stats_t.fram_errors but never rolls back the committed
     * transition. */
    return 0;
}

/* ---------- Unconditional safe-state entry ----------
   try_transition() returns -1 both when a transition is refused AND when
   laststates_log() fails - and in the latter case it returns BEFORE assigning
   current_state (Kilo #23, comment id 3740885211). So on a Flash-sick,
   parity-faulted boot the OBSW used to stay in whatever state it had just
   entered, running nominal ops on memory whose integrity is explicitly not
   established, accompanied by a very well documented flag saying we intended
   otherwise. The LastStates record is best effort; the containment is not, so
   force the state through the SEU pair when the recorded transition does not
   take.

   The RESULT is reported to the caller (Kilo #26, roast 8). While this
   returned void, "CRIT entered and the transition is in the LastStates trail"
   and "CRIT forced by hand because laststates_log() failed" were the same
   answer, and the only consumer - the acknowledgement of the reset-stable
   SRAM2 boot-fault flag - therefore cleared the finding in both cases. That
   is the fail-OPEN direction on the one flag whose whole job is to survive a
   reset: the record never landed, the flag was gone, so the next boot came up
   nominal with no trace on the ground of either the finding or the safe state
   it was supposed to force. */
#define SAFE_STATE_RECORDED   (0)   /* in CRIT, and the record landed        */
#define SAFE_STATE_FORCED    (-1)   /* in CRIT, but nothing was persisted    */

static int enter_safe_state(uint8_t trigger)
{
    if (try_transition(STATE_CRIT, trigger) == 0) {
        return SAFE_STATE_RECORDED;
    }

    seu_mitigation_lock();
    obsw_state.current_state = STATE_CRIT;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    /* Containment must survive a reboot, and the SRAM2 shadow provably does
     * not (sram2_parity_init() erases it at every boot): the caller persists
     * the forced CRIT through to the FRAM golden copy AFTER releasing
     * state_mutex, so seu_mitigation_init() restores CRIT - not the
     * pre-fault state - after a parity-NMI reset. Best effort: a FRAM
     * failure is counted, never rolled back; the containment in RAM stands
     * either way. (No sync here: blocking I2C must not run under
     * state_mutex - see try_transition().) */

    /* Containment is in force either way - only the evidence is missing. */
    return SAFE_STATE_FORCED;
}

/* ---------- FRAM write-through ----------
   Best-effort persistence of the committed OBSW state to the FRAM golden
   copy (W2-5). Runs a blocking I2C transaction (up to 1 s timeout): task
   context only, and NEVER while holding state_mutex or the SEU lock - a
   sick bus must stall one write-through, never the state-transition
   critical section. Call only after the mutex is released. */
static void state_fram_sync(void)
{
    (void)seu_mitigation_sync(SEU_REGION_OBSW_STATE);
}

/* ---------- Battery snapshot refresh ----------

   Connects the two halves of the EPS link that used to run in parallel:
   App/bms/bms.c owns the SPI transaction and the cached reading, while this
   module's critical state holds the snapshot the gates read. Nothing joined
   them, so obsw_state.bms kept its static initialiser for the whole mission
   and every SoC comparison was against a constant.

   Called at 1 Hz from the task loop - not at the loop's 10 Hz - because it
   performs a blocking SPI transaction, and the battery moves on a timescale
   of minutes.

   The copy happens ONLY after a successful poll. A failed poll leaves the
   snapshot exactly as it is: it must never be overwritten with a guess.
   bms.c keeps its own last-known reading and reports valid=false when it
   has none, so the gates stay closed rather than assuming a full battery.
   (The boot state is soc=0/valid=false for the same reason.)

   No FRAM write-through here, unlike bms_set_soc_stub(): the snapshot is
   re-derived from the EPS at every boot, so persisting it every second
   would buy nothing and cost a blocking I2C write in the task loop. The
   SEU lock + commit is still required - this mutates parity-protected
   SRAM2 that the scrub task votes against. */
static void bms_refresh_snapshot(void)
{
    bms_status_t fresh;

    if (bms_poll() != 0) {
        return;   /* no fresh telemetry: keep the snapshot, never invent one */
    }
    fresh = bms_get_status();

    seu_mitigation_lock();
    obsw_state.bms = fresh;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
}

/* ---------- Autonomous battery check ----------
   Returns 1 when a transition was committed (caller must run
   state_fram_sync() after releasing state_mutex), 0 otherwise.

   Every branch below is driven by the SoC BAND from bms.h, so the battery
   handling matches the SPF in one place instead of via threshold literals
   scattered through the state machine:
     - SPF r.600/r.807/r.808 — B_SCRIT / B_CRIT / B_COMMOK are all s2 (CRIT)
       entries. B_CRIT and B_COMMOK were previously DECLARED but never
       enforced anywhere (only b_scrit was read), so a battery sliding from
       79 % down to 26 % produced no protective response at all. They are
       enforced now.
     - SPF r.658 — in s4 (ACTIVE) a fall to B_COMMOK suspends the ongoing
       payload tasks and transitions to s2. The OBSW has no payload-task
       suspend primitive yet, so what this honours is the TRANSITION, which
       is the safety-relevant half: the satellite stops running in ACTIVE on
       a battery it may not be able to sustain. Suspending the individual
       payloads is tracked with the payload workstream.
     - SPF r.613/r.816 — recovery to s3 requires SoC >= B_OPOK AND no active
       critical events; the parity/CRIT latch is what carries "no active
       critical events" (see try_transition()).

   NOT enforced here, deliberately: the SPF's "SoC <= B_CRIT -> after the
   current PDT" refinement (r.807). The OBSW has no PDT scheduler yet, so
   the ordering the SPF describes cannot be honoured; entering CRIT
   immediately is the conservative side of that requirement and is what
   this does. It is not a substitute for the PDT ordering once PDTs exist.

   OPEN DECISION (not silently resolved here): the case "already in s4
   (ACTIVE) and the SoC becomes UNKNOWN" - i.e. the EPS link is lost while
   payloads are running. Nothing below fires, so the payloads keep running
   on a battery nobody is measuring. Containing to CRIT would be the
   conservative answer, but SPF r.805 enumerates the s2 entry conditions and
   "the battery could not be read" is not among them, so implementing it
   means either reporting it as a critical event or amending the SPF - both
   are decisions for the team, not things to invent in the driver. The
   SoC-GATED paths (recovery to s3, manual activation) are already closed by
   bms_soc_allows_payload(), so an unknown SoC cannot START payload ops. */
static int check_battery_autonomous(void)
{
    bms_status_t     bms  = obsw_bms_snapshot();
    bms_soc_band_t   band = bms_soc_band(&bms, &default_thresholds);

    if (bms_soc_is_low(band)) {
        return (try_transition(STATE_CRIT, TRIGGER_BATTERY_LOW) == 0);
    } else if (obsw_state.current_state == STATE_CRIT &&
               bms_soc_allows_payload(&bms, &default_thresholds)) {
        /* No parity check here any more: it lives in try_transition(), which
           covers this caller and every other one (Kilo #23, id 3740885216).
           A refused recovery simply leaves the OBSW in CRIT. */
        return (try_transition(STATE_READY, TRIGGER_BATTERY_OK) == 0);
    }
    return 0;
}

/* ---------- Main task loop ---------- */
static void state_machine_task(void *arg)
{
    /* Outcome of the parity safe-state entry, consumed by the acknowledgement
       below (Kilo #26, roast 8). "_entered" records that the parity branch is
       the one that ran at all - the boot-CRC branch takes priority and enters
       CRIT under a different trigger. */
    int parity_safe_entered = 0;
    int parity_safe_rc = SAFE_STATE_FORCED;
    /* Set when a boot transition committed: the FRAM write-through runs
     * AFTER the matching osMutexRelease (blocking I2C under state_mutex
     * would wedge the transition critical section). */
    int boot_dirty = 0;
    int init_committed;
    /* Battery-refresh divider: the loop runs at 10 Hz, the EPS poll at
       1 Hz (see bms_refresh_snapshot()). */
    unsigned bms_tick = 0U;

    (void)arg;

    /* Boot sequence: s0 → s1 → s3 */
    osDelay(pdMS_TO_TICKS(100));   /* let peripherals settle */

    osMutexAcquire(state_mutex, osWaitForever);
    init_committed = (try_transition(STATE_INIT, TRIGGER_BOOT) == 0);
    osMutexRelease(state_mutex);
    if (init_committed) {
        state_fram_sync();
    }

    /* TODO: antenna deployment sequence + self-tests here */
    osDelay(pdMS_TO_TICKS(500));   /* placeholder for init work */

    osMutexAcquire(state_mutex, osWaitForever);

    /* Latch the SRAM2 parity finding BEFORE any boot transition is attempted.
       The confinement in try_transition() has to be in force when the READY
       transition is evaluated, otherwise the OBSW enters READY first and is
       only dragged back afterwards - which is exactly what happened when the
       latch was set after the boot transition. */
    if (sram2_parity_boot_fault() != 0) {
        parity_safe_latched = 1U;
    }

    if (boot_crc_image_trusted() == 0) {
        /* Image integrity fault survived the reset budget: enter the safe
           state instead of nominal ops (beacon-only, payloads inhibited). */
        (void)enter_safe_state(TRIGGER_IMAGE_CRC_FAIL);
        boot_dirty = 1;
    } else if (parity_safe_latched != 0U) {
        /* SRAM2 reported a parity finding, or the boot erase never completed:
           come up in the safe state, not in READY on data whose integrity is
           not established. */
        parity_safe_rc = enter_safe_state(TRIGGER_SRAM2_PARITY);
        parity_safe_entered = 1;
        boot_dirty = 1;
    } else {
        boot_dirty = (try_transition(STATE_READY, TRIGGER_ANTENNA_DONE) == 0);
    }

    if (parity_safe_latched != 0U) {
        /* Release the reset-stable half of the finding ONLY when the safe
           state was entered *under the parity trigger* and the transition
           actually reached the LastStates pool. Releasing it only HERE - and
           not in sram2_parity_init() - is what makes the evidence survive
           dual_bank_init()'s OB_Launch, boot_crc_apply_policy()'s reset
           budget and any fault handler in between (Kilo #23, id 3740885202).
           parity_safe_latched stays set for this run either way, so the
           confinement in try_transition() outlives the acknowledgement.

           The two conditions below are the roast-8 fix (Kilo #26). The old
           code acked unconditionally, on the strength of a void-returning
           enter_safe_state():

           - SAFE_STATE_FORCED means laststates_log() failed, so CRIT was
             forced by hand and NOTHING was persisted. Clearing the only
             reset-surviving copy of the finding at that point erases the last
             trace of it: the next boot comes up nominal, on memory whose
             integrity was never re-established, with an empty trail. Keeping
             the flag makes the next boot re-derive the confinement and leaves
             the finding on the telemetry stream - fail-closed, which is the
             only acceptable direction for a safety latch.
           - parity_safe_entered == 0 means the boot-CRC branch above won the
             race and CRIT was recorded under TRIGGER_IMAGE_CRC_FAIL. The
             parity finding then has no record of its own anywhere, so it has
             not been "handled" and must not be retired either. That boot is
             heading for boot_crc_apply_policy()'s reset budget anyway. */
        if ((parity_safe_entered != 0) && (parity_safe_rc == SAFE_STATE_RECORDED)) {
            sram2_parity_boot_fault_ack();
        }
    }

    osMutexRelease(state_mutex);
    if (boot_dirty) {
        /* FRAM write-through for the boot transition, outside state_mutex:
           blocking I2C must not wedge the transition critical section. */
        state_fram_sync();
    }

    /* 10 Hz main loop */
    for (;;) {
        int batt_dirty;
        watchdog_alive_self();

        /* Battery refresh at 1 Hz (every 10th 100 ms tick): blocking SPI to
           the EPS, and the battery moves on a timescale of minutes. Runs
           OUTSIDE state_mutex - it touches only the SRAM2 snapshot, while
           check_battery_autonomous() below takes the lock to read it. */
        if (bms_tick++ >= 10U) {
            bms_tick = 0U;
            bms_refresh_snapshot();
        }

        osMutexAcquire(state_mutex, osWaitForever);
        batt_dirty = check_battery_autonomous();
        osMutexRelease(state_mutex);
        if (batt_dirty) {
            state_fram_sync();
        }

        osDelay(pdMS_TO_TICKS(100));
    }
}

/* ---------- Public functions ---------- */

void state_machine_init(void)
{
    const osMutexAttr_t mtx_attrs = {
        .name      = "stateMtx",
        .attr_bits = osMutexPrioInherit,
    };
    state_mutex = osMutexNew(&mtx_attrs);

    /* sram2_parity_init() copies the .sram2 image out of Flash before the
       application starts. A wrong magic means that copy never happened (or
       the block is corrupted), so restore the compile-time defaults rather
       than running the mission on undefined data. */
    if (obsw_state.magic != OBSW_STATE_MAGIC) {
        (void)sram2_restore_from_image(&obsw_state, sizeof(obsw_state));
    }

    obsw_state.current_state            = STATE_OFF;
    obsw_state.beacon_interval_override = 0U;

    /* (T1.6 scrub-unify) The retired parallel scrubber was removed in favour
     * of seu_mitigation, which auto-registers obsw_state in its own init via
     * state_machine_critical_region(). No manual region registration is
     * needed here any more. */

    /* seu_mitigation_init() runs after this function and takes the first
       snapshot; the commit here is a no-op before that point and keeps the
       shadow in step if the state machine is ever re-initialised. */
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
}

osThreadId_t state_machine_task_create(void)
{
    sm_task_handle = osThreadNew(state_machine_task, NULL, &sm_task_attrs);
    if (sm_task_handle != NULL) {
        (void)watchdog_register_task(sm_task_handle, WDG_PERIOD_STATE_MACHINE_MS);
    }
    return sm_task_handle;
}

obw_state_t state_machine_get_state(void)
{
    obw_state_t s;
    osMutexAcquire(state_mutex, osWaitForever);
    s = obsw_state.current_state;
    osMutexRelease(state_mutex);
    return s;
}

int state_machine_request_transition(obw_state_t target, uint8_t trigger)
{
    int rc;
    osMutexAcquire(state_mutex, osWaitForever);
    rc = try_transition(target, trigger);
    osMutexRelease(state_mutex);
    if (rc == 0) {
        /* FRAM write-through for the committed transition, outside
           state_mutex: the sync runs a blocking I2C transaction (up to 1 s
           timeout) that must not stall the transition critical section. */
        state_fram_sync();
    }
    return rc;
}

uint32_t state_machine_get_beacon_interval(void)
{
    uint32_t interval;
    osMutexAcquire(state_mutex, osWaitForever);
    if (obsw_state.beacon_interval_override != 0) {
        interval = obsw_state.beacon_interval_override;
    } else {
        obw_state_t s = obsw_state.current_state;
        switch (s) {
        case STATE_CRIT:   interval = BEACON_INTERVAL_CRIT;   break;
        case STATE_ACTIVE: interval = BEACON_INTERVAL_ACTIVE;  break;
        case STATE_READY:  interval = BEACON_INTERVAL_READY;   break;
        default:           interval = BEACON_INTERVAL_CRIT;     break;
        }
    }
    osMutexRelease(state_mutex);
    return interval;
}

int state_machine_set_beacon_interval(uint32_t interval_ms)
{
    /* Range check before the value can reach the beacon task or the watchdog.
       0 is the documented "clear the override / revert to per-state default"
       encoding. Any other value must be within the certified band: an
       interval longer than BEACON_INTERVAL_MAX would out-run the beacon
       watchdog period and permanently flag a healthy task, and one shorter
       than BEACON_INTERVAL_MIN would violate the RF duty-cycle budget.
       Out-of-range requests are rejected, leaving the current cadence intact
       (fail-safe: an erroneous or corrupted uplink cannot silence the beacon). */
    if ((interval_ms != 0u) &&
        ((interval_ms < BEACON_INTERVAL_MIN) || (interval_ms > BEACON_INTERVAL_MAX))) {
        return -1;
    }

    osMutexAcquire(state_mutex, osWaitForever);
    seu_mitigation_lock();
    obsw_state.beacon_interval_override = interval_ms;
    (void)seu_mitigation_commit(SEU_REGION_OBSW_STATE);
    seu_mitigation_unlock();
    osMutexRelease(state_mutex);
    /* Write-through to the FRAM golden copy (W2-5): without it a reboot
     * would re-snapshot the previous cadence and the scrubber would repair
     * this commanded change away. Outside the SEU lock AND outside
     * state_mutex: blocking I2C (up to 1 s timeout) must never wedge the
     * OBSW main thread. Best effort, never rolls back. */
    (void)seu_mitigation_sync(SEU_REGION_OBSW_STATE);
    return 0;
}
