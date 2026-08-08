/**
  ******************************************************************************
  * @file    sram2_parity.c
  * @brief   SRAM2 hardware-parity protection for critical OBSW data (W2-3).
  *
  * The STM32L496VGTx has two RAM blocks: SRAM1 (256 KB @ 0x20000000, no error
  * detection) and SRAM2 (64 KB @ 0x10000000) whose every byte carries a parity
  * bit. When the parity check is enabled, a read of a corrupted byte asserts
  * the SRAM2 parity error flag (SYSCFG_CFGR2.SPF) and raises a non-maskable
  * interrupt. Left unhandled, that NMI spins forever in the CubeMX default
  * handler; on orbit that is an unobservable hang until the external watchdog
  * fires and the corruption cause is lost.
  *
  * This module:
  *   1. prepares SRAM2 at boot - hardware erase (which writes valid parity for
  *      all 64 KB) followed by a copy of the initialised .sram2 image from
  *      Flash, so no read can ever hit a location with undefined parity,
  *   2. reports whether the SRAM2_PE user option byte actually enables the
  *      hardware check (it is a non-volatile setting, not a register bit),
  *   3. turns the parity NMI into a recorded, deterministic reboot: capture
  *      SYSCFG / FLASH / RCC / SCB status into a LastStates entry, then
  *      NVIC_SystemReset() - the same containment pattern the Cortex-M fault
  *      handlers use.
  *
  * Enabling notes (RM0351 rev 9, §3.4 and §2.2.2):
  *   - FLASH_OPTR.SRAM2_PE is active low: 0 = parity check ENABLED,
  *     1 = disabled (the reset value on a virgin part is 1).
  *   - FLASH_OPTR.SRAM2_RST erases SRAM2 on every system reset when 0.
  *   - The parity error signal can additionally be routed and locked onto the
  *     TIM1/8/15/16/17 break inputs (SYSCFG_CFGR2.SPL) so that PWM actuators
  *     are forced to a safe state; that is opt-in here because the lock is
  *     only released by a system reset.
  *
  * Standards: NASA-STD-8739.8 (data integrity, fault containment, no silent
  *            failure), ECSS-E-ST-40C (recorded failure context),
  *            ECSS-Q-ST-80C (fault detection).
  ******************************************************************************
  */

#include "sram2_parity.h"

#include "main.h"          /* HAL + CMSIS core (SYSCFG, FLASH, SCB, NVIC)    */
#include "memory.h"        /* laststates_write()                             */
#include "obsw_types.h"    /* laststates_entry_t, TRIGGER_SRAM2_PARITY       */

#include <stddef.h>
#include <string.h>

/* ---------- Linker-provided SRAM2 layout (STM32L496VGTX_FLASH.ld) ---------- */
extern uint32_t _sisram2;          /* .sram2 load image in Flash             */
extern uint32_t _ssram2;           /* start of .sram2        (initialised)   */
extern uint32_t _esram2;           /* end   of .sram2                        */
extern uint32_t _ssram2_noinit;    /* start of .sram2_noinit (zeroed by HW)  */
extern uint32_t _esram2_noinit;    /* end   of .sram2_noinit                 */
extern uint32_t _sram2_region_start;
extern uint32_t _sram2_region_end;

/* The record must survive the trip through a LastStates entry unchanged. */
_Static_assert(sizeof(sram2_parity_record_t) <=
                   sizeof(((laststates_entry_t *)0)->context),
               "sram2_parity_record_t does not fit in a LastStates context blob");

/* A parity NMI can hit in any operational state, and state_machine_get_state()
   takes a mutex, so it must never be called from an exception handler. The
   state fields of the LastStates entry are therefore marked unknown - the same
   convention the Cortex-M fault handlers use. */
#define SRAM2_STATE_UNKNOWN     0xFFU

/* ---------- Bounded waits for the SRAM2 hardware erase ----------
   Fixed iteration counts, not HAL_GetTick() timeouts: sram2_parity_init()
   runs before the RTOS (SysTick is not started yet) and on the NMI path
   SysTick cannot preempt the handler, so a tick-based timeout can never
   expire.

   Sizing (RM0351 rev 9, SYSCFG_SCSR): the hardware erase clears the whole
   64 KB block at one 32-bit word per AHB cycle, i.e. 64 KB / 4 B = 16 384
   cycles ~ 4.1 ms at the MSI 4 MHz reset clock this code runs on
   (SystemClock_Config() has not run yet). One poll iteration is an APB2 load
   plus loop overhead, i.e. >= 8 CPU cycles even at -O0, so 20 000 iterations
   cover >= 160 000 cycles ~ 40 ms at 4 MHz: a ~10x margin over the documented
   erase time. The previous 1 000 000 iterations were a multi-second boot
   stall on the failure path, with no watchdog running to end it.

   SRAM2_ERASE_START_POLL_LIMIT only bounds the *acknowledge* of the request:
   the store to SCSR is posted on APB2, so a handful of iterations suffices to
   observe SRAM2ER (or SRAM2BSY) come up. */
#define SRAM2_ERASE_POLL_LIMIT        20000UL
#define SRAM2_ERASE_START_POLL_LIMIT  1000UL

/* Cycle-counted bound for the Flash controller on the NMI path. ~52 ms at
   80 MHz, longer than a 2 KB page erase (22 ms nominal), same budget the
   LastStates writer in App/memory/memory.c uses. */
#define SRAM2_FLASH_IDLE_TIMEOUT_CYCLES  0x400000U

/* Number of boot-time findings that can be queued for deferred persistence.
   Two is enough for the worst case (parity flag latched at boot + the erase
   that follows it failing). */
#define SRAM2_PENDING_MAX       2U

/* How many times boot may reboot on an SRAM2 erase that never finishes before
   it gives up and continues into the safe state instead. Two attempts cover a
   transient (a debugger poking SYSCFG, a half-completed erase across a warm
   reset); beyond that the engine is wedged and rebooting forever would only
   trade a diagnosable STATE_CRIT for a silent spacecraft. Counted in the
   reset-stable store, because a .bss counter is zeroed by the very reset it
   is supposed to be counting. */
#define SRAM2_ERASE_BUSY_RESET_LIMIT  2U

/* How many LastStates records an RCC clock-security (CSS) failure may burn.
   A dead crystal is permanent, the NMI is level-triggered off CSSF and the
   hardware keeps running on the HSI fallback, so an unbounded record path
   would program the whole pool away and destroy the forensic trail it is
   meant to preserve. Counted in the reset-stable store because a warm reset
   from any other subsystem must not re-arm the budget. */
#define SRAM2_CLOCK_FAULT_RECORD_LIMIT  3U

/* ---------- Reset-stable fault store (SRAM1, .noinit) ----------
   A parity NMI records the event and resets, so whatever the next boot needs
   to know has to survive that reset. A flag in .bss cannot: the startup code
   zero-fills .bss on every reset, and SYSCFG_CFGR2.SPF is cleared both by the
   handler and by the reset itself. That is why the advertised chain "parity
   NMI -> record -> reset -> come up in the safe state" used to land in the
   nominal branch instead (Kilo C on state_machine.c).

   The store therefore lives in .noinit, which the linker places after .bss in
   SRAM1 (STM32L496VGTX_FLASH.ld) and which startup_stm32l496vgtx.s does not
   clear, so its contents survive a system reset; only a power-on or
   brown-out reset destroys them. It is deliberately NOT in SRAM2: memory that
   has just reported a parity error must not be trusted to carry its own
   post-mortem. RTC backup registers would be the other candidate, but the RTC
   is not clocked in this build (HAL_RTC_MODULE_ENABLED is off and no
   backup-domain access is enabled), so they would always read back as zero.

   Validity is a magic plus its complement AND a checksum over the payload
   words - the same belt-and-braces the MPU post-mortem record uses
   (mpu_fault_record_t.chk, Core/Src/mpu.c). The magic pair catches a reset
   that caught the struct half-written; the checksum catches the event this
   whole module exists for, namely an upset in the bookkeeping itself. Without
   it a single 1->0 flip in boot_fault silently retires a parity finding -
   fail-OPEN, the one direction a safe-state latch must never fail (Kilo #23,
   comment id 3740885206).

   The object is volatile: the NMI handler writes it asynchronously to
   whatever the foreground was doing, so the compiler must not cache the
   fields or sink the stores past the __DSB() that publishes them. That also
   rules out memset() on it, hence the field-wise clearing below.

   A store that fails validation is NOT blindly zeroed either. parity_resets /
   dropped_records / erase_busy_resets are the history ground needs to tell
   "one upset" from "this cell has died", so they are salvaged whenever the
   struct was recognisably ours (at least one magic word intact) and the value
   is still plausible. An implausible value is SATURATED at
   SRAM2_STORE_COUNT_SANE, never reset to 0 (Kilo #26, comment id 3741110977):
   zeroing deletes the very evidence the salvage exists to protect, and for
   erase_busy_resets it also hands the wedged-erase path a brand new
   SRAM2_ERASE_BUSY_RESET_LIMIT budget on exactly the corrupted-memory boots
   where an unbounded reset loop is least affordable - the recovery disarming
   the bound. "Implausibly large" still carries signal; 0 carries none.
   Only a store with BOTH magic words wrong is treated as
   a cold start: that is indistinguishable from uninitialised .noinit after a
   power-on / brown-out reset, and believing a garbage boot_fault there would
   park a perfectly healthy spacecraft in STATE_CRIT at first switch-on.

   The store is also VERSIONED (Kilo #26, comment id 3741226504). The magic
   pair only says "these words are ours"; it says nothing about their layout.
   Every time a field was appended - erase_busy_resets, then clock_faults /
   first_busy_scsr / first_busy_cfgr2 - the checksum changed shape, so a store
   written by the PREVIOUS image failed sram2_store_is_valid() on the first
   boot after a firmware update and the recovery below threw away every
   salvaged counter: a reset-stable store that does not survive an OTA is not
   reset-stable, it is just slow to lose data. The version word makes the
   layout explicit, and sram2_store_recover() recognises the pre-version
   layout and MIGRATES it (authenticating it with the old checksum formula
   when that layout carried one) instead of calling it garbage. Bump
   SRAM2_STORE_VERSION on any future field change and teach the migration
   path about the layout it replaces. */
#define SRAM2_STORE_MAGIC       0x53324653U   /* "S2FS" */

/* Layout id of sram2_fault_store_t. Version 1 is every pre-version layout
   (no version word): the 6-word shape that flew first (magic pair, boot_fault,
   last_event, parity_resets, dropped_records - no checksum at all) and the
   8-word shape that added erase_busy_resets + chk. Version 2 is the struct
   below. */
#define SRAM2_STORE_VERSION     2U

/* Counter values above this are not credible for a single mission run, so a
   salvaged word beyond it is corruption rather than history. A uniformly
   random 32-bit word lands above it with probability ~0.99998. */
#define SRAM2_STORE_COUNT_SANE  100000U

typedef struct {
    uint32_t magic;             /* SRAM2_STORE_MAGIC                        */
    uint32_t magic_inv;         /* ~SRAM2_STORE_MAGIC                       */
    uint32_t version;           /* SRAM2_STORE_VERSION: layout marker       */
    uint32_t boot_fault;        /* 1: last run ended on a parity finding    */
    uint32_t last_event;        /* SRAM2_EVENT_* of that finding            */
    uint32_t parity_resets;     /* parity-triggered resets since power-on   */
    uint32_t dropped_records;   /* LastStates writes the fault path lost    */
    uint32_t erase_busy_resets; /* resets taken on an SRAM2 erase still busy */
    uint32_t clock_faults;      /* RCC CSS NMIs seen (record budget)        */
    uint32_t first_busy_scsr;   /* SYSCFG->SCSR at the FIRST erase-busy hit */
    uint32_t first_busy_cfgr2;  /* SYSCFG->CFGR2 at that same first hit     */
    uint32_t chk;               /* XOR of the payload words above           */
} sram2_fault_store_t;

/* The version-1 (pre-version-word) layout, kept ONLY so the first boot after
   a firmware update can read what the previous image left behind. The 6-word
   shape stops after dropped_records, so erase_busy_resets/chk then alias
   whatever follows in .noinit and must not be believed unless chk validates
   them (see sram2_store_v1_checksum()). */
typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
    uint32_t boot_fault;
    uint32_t last_event;
    uint32_t parity_resets;
    uint32_t dropped_records;
    uint32_t erase_busy_resets;
    uint32_t chk;
} sram2_fault_store_v1_t;

/* One object, two views. A union (rather than a cast of &sram2_store) keeps
   both layouts at the same .noinit address without type-punning through an
   incompatible pointer. */
typedef union {
    sram2_fault_store_t    v2;
    sram2_fault_store_v1_t v1;
} sram2_store_image_t;

static volatile sram2_store_image_t sram2_store_image
    __attribute__((section(".noinit"), used));

/* Every access site in this file reads and writes the CURRENT layout. */
#define sram2_store             (sram2_store_image.v2)

/* ---------- Module state ----------
   Deliberately in SRAM1 (default .bss / .data): memory that has just reported
   a parity error must not be trusted to hold its own diagnostics. */
static volatile uint32_t sram2_parity_events = 0U;
static sram2_parity_status_t sram2_status = SRAM2_PARITY_STATUS_UNKNOWN;

/* Boot-time records waiting for the LastStates pool to be initialised, and
   the flag that makes the boot fault visible to the state machine. */
static laststates_entry_t sram2_pending[SRAM2_PENDING_MAX];
static uint32_t sram2_pending_count = 0U;
static uint8_t  sram2_boot_fault_flag = 0U;

/* 0 once the SRAM2 hardware erase has been found still running after both
   bounded waits AND the reboot budget is spent: the block is under the erase
   engine, so it must not be read (parity NMI) nor written (the erase zeroes
   it afterwards with valid parity). Latched for the rest of the boot and
   published through sram2_parity_region_usable(); see that declaration.
   Defaults to 1 so a build that never calls sram2_parity_init() - the host
   unit tests - behaves exactly as before. */
static uint8_t  sram2_region_usable = 1U;

/* ---------- Helpers ---------- */

static uint32_t sram2_store_checksum(void)
{
    return sram2_store.version         ^ sram2_store.boot_fault ^
           sram2_store.last_event      ^ sram2_store.parity_resets ^
           sram2_store.dropped_records ^ sram2_store.erase_busy_resets ^
           sram2_store.clock_faults    ^ sram2_store.first_busy_scsr ^
           sram2_store.first_busy_cfgr2 ^
           SRAM2_STORE_MAGIC;
}

/* The checksum formula of the version-1 layout (payload words only, no
   version word, three fields short). Used exclusively to authenticate a store
   left behind by the previous firmware image before migrating it: a match
   means those words really are the old store and not .noinit debris that
   happens to carry our magic. */
static uint32_t sram2_store_v1_checksum(void)
{
    return sram2_store_image.v1.boot_fault        ^
           sram2_store_image.v1.last_event        ^
           sram2_store_image.v1.parity_resets     ^
           sram2_store_image.v1.dropped_records   ^
           sram2_store_image.v1.erase_busy_resets ^
           SRAM2_STORE_MAGIC;
}

/* Publish the payload: stamp the checksum, then a barrier so the whole store
   is visible before the caller resets or returns to the interrupted code. */
static void sram2_store_seal(void)
{
    sram2_store.chk = sram2_store_checksum();
    __DSB();
}

static int sram2_store_is_valid(void)
{
    return ((sram2_store.magic == SRAM2_STORE_MAGIC) &&
            (sram2_store.magic_inv == (uint32_t)~SRAM2_STORE_MAGIC) &&
            (sram2_store.version == SRAM2_STORE_VERSION) &&
            (sram2_store.chk == sram2_store_checksum())) ? 1 : 0;
}

/* Re-establish a valid store after a failed validation, preserving as much
   history as can still be believed (see the section comment above). */
static void sram2_store_recover(void)
{
    uint32_t fault   = 0U;
    uint32_t event   = (uint32_t)SRAM2_EVENT_NONE;
    uint32_t resets  = 0U;
    uint32_t dropped = 0U;
    uint32_t busy    = 0U;
    uint32_t clocks  = 0U;
    uint32_t bscsr   = 0U;
    uint32_t bcfgr2  = 0U;
    int magic_words  = 0;

    if (sram2_store.magic == SRAM2_STORE_MAGIC)                  { magic_words++; }
    if (sram2_store.magic_inv == (uint32_t)~SRAM2_STORE_MAGIC)   { magic_words++; }

    if (magic_words != 0) {
        if (sram2_store.version != SRAM2_STORE_VERSION) {
            /* ---- Pre-version layout: a firmware update, not corruption ----
               The words are ours (magic) but they were written by an image
               whose struct was three fields shorter, so the current checksum
               could never match. Treating that as garbage silently dropped
               every counter on the first boot after every OTA - the "survives
               a reset" store not surviving a firmware update (Kilo #26,
               comment id 3741226504). Migrate instead.

               erase_busy_resets only exists in the 8-word shape of version 1;
               in the original 6-word shape that word aliases unrelated
               .noinit content, so it is carried over ONLY when the old
               checksum authenticates it. Zeroing it there is not the
               evidence-deleting move roast 3741110977 is about: the counter
               genuinely did not exist in that layout, so there is no history
               to preserve and no budget being re-armed by a corruption. */
            const int v1_authentic =
                (magic_words == 2) &&
                (sram2_store_image.v1.chk == sram2_store_v1_checksum());

            resets  = sram2_store_image.v1.parity_resets;
            dropped = sram2_store_image.v1.dropped_records;
            event   = sram2_store_image.v1.last_event;
            fault   = (sram2_store_image.v1.boot_fault != 0U) ? 1U : 0U;
            busy    = (v1_authentic != 0)
                          ? sram2_store_image.v1.erase_busy_resets : 0U;
            /* clock_faults / first_busy_* had no equivalent in version 1. */
            clocks  = 0U;
            bscsr   = 0U;
            bcfgr2  = 0U;

            if (resets  > SRAM2_STORE_COUNT_SANE) { resets  = SRAM2_STORE_COUNT_SANE; }
            if (dropped > SRAM2_STORE_COUNT_SANE) { dropped = SRAM2_STORE_COUNT_SANE; }
            if (busy    > SRAM2_STORE_COUNT_SANE) { busy    = SRAM2_STORE_COUNT_SANE; }

            if (event > (uint32_t)SRAM2_EVENT_LAST) {
                event = (uint32_t)SRAM2_EVENT_STORE_LOST;
            } else if ((event == (uint32_t)SRAM2_EVENT_PARITY_NMI) &&
                       (fault == 0U) && (resets == 0U)) {
                /* Version 1 had no SRAM2_EVENT_NONE: id 0 meant BOTH "a
                   parity NMI was recorded" and "nothing has ever been
                   recorded" (the cold-start store was all zeros). Migrating
                   that ambiguity verbatim would re-import the exact fail-open
                   this branch removed - a later checksum failure would read
                   event == PARITY_NMI, corroborate the flag out of silence
                   and park a healthy spacecraft in STATE_CRIT (Kilo #26,
                   comment id 3741110976).

                   boot_fault alone CANNOT disambiguate the two, though: the
                   deployed version-1 image consumes the latch at boot
                   (sram2_boot_fault_flag = 1U; sram2_store.boot_fault = 0U)
                   and leaves last_event at PARITY_NMI for the rest of the
                   mission, so the steady state of every v1 unit that has ever
                   been bitten is exactly (boot_fault == 0, last_event == 0,
                   parity_resets > 0) - the tuple this branch used to re-label
                   as "nothing has ever been recorded", reporting a unit with a
                   real parity hit as pristine (Kilo #26, comment id
                   3741302889).

                   So let the salvaged reset counter vote: v1 bumped
                   parity_resets only in sram2_store_note_fault(), i.e. only on
                   a real parity NMI, so resets > 0 is positive evidence that
                   the id is a genuine (already acked) historical NMI and it
                   migrates verbatim. Only silence with no resets behind it is
                   really silence, and only that is re-labelled - corroboration
                   still never fires on a cold-start store, and the evidence of
                   a real hit is never erased. */
                event = (uint32_t)SRAM2_EVENT_NONE;
            }
            /* Any other recorded event id survives the migration unchanged:
               ids 0..6 mean the same thing in both layouts. */
        } else {
            /* The struct was recognisably ours AND laid out the way this image
               expects, so an upset hit the magic pair and/or the payload rather
               than this being a cold start. Salvage the counters within a
               plausibility bound instead of wiping the "one upset vs dead cell"
               evidence, and resolve boot_fault upwards: a finding that may exist
               is treated as a finding, because losing it is the fail-open
               direction. */
            resets  = sram2_store.parity_resets;
            dropped = sram2_store.dropped_records;
            busy    = sram2_store.erase_busy_resets;
            clocks  = sram2_store.clock_faults;
            event   = sram2_store.last_event;
            fault   = (sram2_store.boot_fault != 0U) ? 1U : 0U;

            /* SATURATE, never zero (Kilo #26, comment id 3741110977). An
               implausible value is still evidence that something went badly
               wrong here; 0 is not, and for erase_busy_resets it would re-arm
               the very reboot budget that keeps a wedged erase engine from
               turning the spacecraft into a reset loop. */
            if (resets  > SRAM2_STORE_COUNT_SANE) { resets  = SRAM2_STORE_COUNT_SANE; }
            if (dropped > SRAM2_STORE_COUNT_SANE) { dropped = SRAM2_STORE_COUNT_SANE; }
            if (busy    > SRAM2_STORE_COUNT_SANE) { busy    = SRAM2_STORE_COUNT_SANE; }
            if (clocks  > SRAM2_STORE_COUNT_SANE) { clocks  = SRAM2_STORE_COUNT_SANE; }

            /* Register snapshots: every 32-bit value is a legal reading, so there
               is no plausibility bound to apply - carry them through untouched. */
            bscsr  = sram2_store.first_busy_scsr;
            bcfgr2 = sram2_store.first_busy_cfgr2;

            if (event > (uint32_t)SRAM2_EVENT_LAST) {
                event = (uint32_t)SRAM2_EVENT_STORE_LOST;
            } else if (event == (uint32_t)SRAM2_EVENT_PARITY_NMI) {
                /* A GENUINELY recorded parity NMI corroborates the flag. This is
                   only sound because "nothing recorded yet" is SRAM2_EVENT_NONE
                   and not id 0 (Kilo #26, comment id 3741110976): while the two
                   shared the value 0, one upset in dropped_records on a pristine
                   unit failed the checksum, landed here, read event == 0 and
                   forced STATE_CRIT on a perfectly healthy spacecraft. The rule
                   now fires on evidence, not on silence. */
                fault = 1U;
            }
        }
    }

    /* Field-wise, not memset(): the object is volatile. */
    sram2_store.magic             = SRAM2_STORE_MAGIC;
    sram2_store.magic_inv         = (uint32_t)~SRAM2_STORE_MAGIC;
    sram2_store.version           = SRAM2_STORE_VERSION;
    sram2_store.boot_fault        = fault;
    sram2_store.last_event        = event;
    sram2_store.parity_resets     = resets;
    sram2_store.dropped_records   = dropped;
    sram2_store.erase_busy_resets = busy;
    sram2_store.clock_faults      = clocks;
    sram2_store.first_busy_scsr   = bscsr;
    sram2_store.first_busy_cfgr2  = bcfgr2;
    sram2_store_seal();
}

/* Every note_*() helper below is callable from the NMI: no HAL, no lock, no
   Flash - none of them can fail or block, which is what makes the store a
   usable sink when the LastStates write cannot happen. */
static void sram2_store_validate(void)
{
    if (sram2_store_is_valid() == 0) {
        sram2_store_recover();
    }
}

/* Note the event without claiming the next boot: used for findings that are
   already handled inside the boot they are detected in. */
static void sram2_store_note_event(uint32_t event_id)
{
    sram2_store_validate();
    sram2_store.last_event = event_id;
    sram2_store_seal();
}

/* Latch a parity fault for the *next* boot AND count the parity-triggered
   reset that is about to be taken.

   parity_resets is documented as "parity-triggered system resets since the
   last power-on" and is ground's only "one upset vs a dead cell"
   discriminator, so ONLY a real parity NMI that actually resets may call
   this. Anything else must use sram2_store_note_boot_fault() below
   (Kilo #26, comment id 3741110980). */
static void sram2_store_note_fault(uint32_t event_id)
{
    sram2_store_validate();
    sram2_store.boot_fault = 1U;
    sram2_store.last_event = event_id;
    sram2_store.parity_resets++;
    sram2_store_seal();
}

/* Latch a boot fault WITHOUT touching parity_resets: the finding confines the
   OBSW to the safe state on the next boot, but it is neither a parity error
   nor a reset. Padding parity_resets from here inflated the discriminator and
   then reseeded sram2_parity_events from the inflated number on every
   subsequent boot, compounding the lie (Kilo #26, comment id 3741110980). */
static void sram2_store_note_boot_fault(uint32_t event_id)
{
    sram2_store_validate();
    sram2_store.boot_fault = 1U;
    sram2_store.last_event = event_id;
    sram2_store_seal();
}

/* Latch an SRAM2 erase that was still running when the bounded waits expired,
   and count the reset this boot is about to take because of it.

   The register snapshot of the FIRST occurrence is stashed here as well. The
   sram2_parity_record_t built around it lives on the stack and the pending
   queue is in .bss, so both die in the reset below: without this, the
   diagnostically interesting first two occurrences were discarded and only
   the final give-up pass ever reached ground (Kilo #26, id 3741110980). */
static void sram2_store_note_erase_busy(uint32_t scsr, uint32_t cfgr2)
{
    sram2_store_validate();
    if (sram2_store.erase_busy_resets == 0U) {
        sram2_store.first_busy_scsr  = scsr;
        sram2_store.first_busy_cfgr2 = cfgr2;
    }
    sram2_store.boot_fault = 1U;
    sram2_store.last_event = (uint32_t)SRAM2_EVENT_ERASE_BUSY;
    sram2_store.erase_busy_resets++;
    sram2_store_seal();
}

static void sram2_store_note_dropped(void)
{
    sram2_store_validate();
    sram2_store.dropped_records++;
    sram2_store_seal();
}

/* ---------- SRAM2 hardware erase ----------
   sram2_hw_erase() distinguishes its two failure modes because they demand
   OPPOSITE reactions (Kilo #23, comment id 3740885200):

     SRAM2_ERASE_NOT_ACKED  the hardware never took the request, so the block
                            is idle and untouched - filling it by hand is both
                            safe and necessary,
     SRAM2_ERASE_BUSY       the erase is STILL running after the bound. The
                            block is under the erase engine: anything written
                            into it now is zeroed the moment the erase
                            finishes - with valid parity, so the parity check
                            this module exists for never says a word and the
                            OBSW boots on an all-zero critical state that
                            looks pristine.

   Collapsing both into a single -1 and then software-filling was exactly that
   second, silent-corruption case. */
#define SRAM2_ERASE_OK          0
#define SRAM2_ERASE_NOT_ACKED  (-1)
#define SRAM2_ERASE_BUSY       (-2)

static int sram2_hw_erase(void)
{
    const uint32_t busy_mask = SYSCFG_SCSR_SRAM2ER | SYSCFG_SCSR_SRAM2BSY;
    int started = 0;
    int done    = 0;
    int rc;

    /* Unlock the write protection of SCSR.SRAM2ER (key sequence 0xCA, 0x53),
       then start the erase. The hardware writes 0x00 with correct parity to
       every byte of SRAM2. */
    __HAL_SYSCFG_SRAM2_WRP_UNLOCK();
    __HAL_SYSCFG_SRAM2_ERASE();

    /* Fail fast when the request never latched. SRAM2ER == 0 && SRAM2BSY == 0
       is exactly the *idle* state the block sits in before anything is asked
       of it, so polling straight for it cannot tell "the erase finished" from
       "the erase never started": a rejected request (the SKR unlock did not
       take, something else wrote SKR/SCSR in between, a debugger poked
       SYSCFG) would satisfy the completion test on the first iteration and
       the caller would then copy the init image onto a block whose parity
       bits are still undefined. Wait for the hardware to acknowledge first,
       and only then poll for completion. */
    for (uint32_t i = 0U; i < SRAM2_ERASE_START_POLL_LIMIT; i++) {
        if ((SYSCFG->SCSR & busy_mask) != 0U) {
            started = 1;
            break;
        }
    }

    if (started != 0) {
        for (uint32_t i = 0U; i < SRAM2_ERASE_POLL_LIMIT; i++) {
            if ((SYSCFG->SCSR & busy_mask) == 0U) {
                done = 1;
                break;
            }
        }
    }

    /* Symmetry with the unlock above: writing any value that is not the
       0xCA / 0x53 sequence reactivates the write protection of SRAM2ER
       (RM0351 rev 9, SYSCFG_SKR), so no stray write can wipe the parity-
       protected block once the OBSW is running. */
    SYSCFG->SKR = 0x00U;

    if (started == 0) {
        rc = SRAM2_ERASE_NOT_ACKED;
    } else if (done == 0) {
        rc = SRAM2_ERASE_BUSY;
    } else {
        rc = SRAM2_ERASE_OK;
    }

    __DSB();
    return rc;
}

/* Second, longer bounded wait for an erase that had been acknowledged but was
   still running when sram2_hw_erase() gave up. Same fixed-iteration reasoning
   as SRAM2_ERASE_POLL_LIMIT (no SysTick this early), 20x the budget: this is
   the "give the engine every chance before declaring the block unusable"
   path, not the nominal one, so a longer stall here is acceptable - and it is
   still bounded, which is the whole point.
   @retval 0 the block went idle, -1 it is still busy. */
#define SRAM2_ERASE_SETTLE_POLL_LIMIT  400000UL

static int sram2_erase_settle_wait(void)
{
    const uint32_t busy_mask = SYSCFG_SCSR_SRAM2ER | SYSCFG_SCSR_SRAM2BSY;

    for (uint32_t i = 0U; i < SRAM2_ERASE_SETTLE_POLL_LIMIT; i++) {
        if ((SYSCFG->SCSR & busy_mask) == 0U) {
            __DSB();
            return 0;
        }
    }
    return -1;
}

/* Software fallback for an erase the hardware never acknowledged. A store
   also writes the parity bits of the bytes it covers, so filling the whole
   RAM2 region by hand leaves every byte with a defined value and valid parity
   - slower than the hardware erase (16 K stores) but it removes the
   undefined-parity state instead of leaving it for the first read to trip
   over. Without it the failure path is circular: detecting the problem later
   means *reading* SRAM2, and that read is precisely what raises the parity
   NMI this module exists to prevent.

   Only legal when the erase engine is idle - see SRAM2_ERASE_BUSY above. */
static void sram2_sw_fill(void)
{
    volatile uint32_t *p   = (volatile uint32_t *)&_sram2_region_start;
    volatile uint32_t *end = (volatile uint32_t *)&_sram2_region_end;

    while (p < end) {
        *p = 0U;
        p++;
    }
    __DSB();
}

static void sram2_copy_init_image(void)
{
    const uint32_t *src = (const uint32_t *)&_sisram2;
    uint32_t *dst       = (uint32_t *)&_ssram2;
    const uint32_t *end = (const uint32_t *)&_esram2;

    while (dst < end) {
        *dst++ = *src++;
    }
    __DSB();
}

static void sram2_fill_record(sram2_parity_record_t *rec, uint32_t event_id)
{
    memset(rec, 0, sizeof(*rec));
    rec->magic        = SRAM2_PARITY_RECORD_MAGIC;
    rec->event_id     = event_id;
    rec->cfgr2        = SYSCFG->CFGR2;
    rec->scsr         = SYSCFG->SCSR;
    rec->optr         = FLASH->OPTR;
    rec->rcc_cifr     = RCC->CIFR;
    rec->icsr         = SCB->ICSR;
    rec->region_start = (uint32_t)&_sram2_region_start;
    rec->region_end   = (uint32_t)&_sram2_region_end;
    rec->error_count  = sram2_parity_events;
}

/* Wrap a record in a LastStates entry. Every status register is sampled by
   sram2_fill_record() before the flags are cleared, so an entry built here can
   be written to Flash later without losing information.

   The trigger is a PARAMETER, not a hard-coded TRIGGER_SRAM2_PARITY: filing a
   dead HSE crystal under "SRAM2 parity" made ground diagnose memory
   degradation for an oscillator failure (Kilo #26, comment id 3741110986). */
static void sram2_fill_entry(laststates_entry_t *entry,
                             const sram2_parity_record_t *rec,
                             uint8_t trigger)
{
    memset(entry, 0, sizeof(*entry));
    entry->timestamp  = HAL_GetTick();
    entry->state_from = SRAM2_STATE_UNKNOWN;
    entry->state_to   = SRAM2_STATE_UNKNOWN;
    entry->trigger    = trigger;
    memcpy(entry->context, rec, sizeof(*rec));
}

/* Queue a boot-time finding for persistence after laststates_init(). */
static void sram2_queue_boot_record(const sram2_parity_record_t *rec)
{
    if (sram2_pending_count >= SRAM2_PENDING_MAX) {
        return;   /* keep the oldest: it carries the first cause */
    }
    sram2_fill_entry(&sram2_pending[sram2_pending_count], rec,
                     TRIGGER_SRAM2_PARITY);
    sram2_pending_count++;
}

/* Enable the DWT cycle counter. It is driven by the CPU clock and keeps
   counting inside an NMI, unlike SysTick, which cannot preempt one. */
static void sram2_dwt_enable(void)
{
    if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    }
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        DWT->CYCCNT = 0U;
        DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

/* Bring the Flash controller to a state the fault path can actually program
   from.

   Waiting for BSY only proves the controller is not busy *this instant*; it
   proves nothing about the wreckage the preempted task left in FLASH->CR
   (Kilo #23, comment id 3740885203). Preempt flash_write_dword_bounded()
   between its two 32-bit stores (App/memory/memory.c) and BSY is not even set
   yet, so a bare BSY wait returns immediately and the handler's first word is
   latched as the second half of somebody else's double word. Preempt
   flash_erase_page_bounded() and PER+PNB are still armed, so the handler's
   SET_BIT(FLASH->CR, FLASH_CR_PG) means PG on top of PER - a
   programming-sequence error that files the post-mortem under
   dropped_records, in the exact scenario the record was written for.

   So: bounded BSY wait, then clear every operation-select bit plus the page
   number, then the error latches. App/memory/memory.c's
   laststates_flash_isr_prepare() runs the same sequence when it detects the
   ISR context; the writes are idempotent register stores, and doing it here
   too keeps this path correct independently of which sink it hands to.

   @retval 0 the controller is idle and sanitised,
   @retval -1 it stayed busy for longer than SRAM2_FLASH_IDLE_TIMEOUT_CYCLES
              (wedged - refuse the write rather than queue behind an operation
              that may never end). */
static int sram2_flash_prepare(void)
{
    uint32_t start;

    sram2_dwt_enable();
    start = DWT->CYCCNT;

    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY) != RESET) {
        if ((DWT->CYCCNT - start) >= SRAM2_FLASH_IDLE_TIMEOUT_CYCLES) {
            return -1;
        }
    }

    CLEAR_BIT(FLASH->CR, (FLASH_CR_PG | FLASH_CR_FSTPG | FLASH_CR_PER |
                          FLASH_CR_MER1 | FLASH_CR_MER2 | FLASH_CR_PNB));
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    __DSB();

    return 0;
}

/* Serialise the record into the LastStates pool (internal Flash).

   Bounded by construction, which matters because this also runs in the NMI:
   App/memory/memory.c programs and erases through flash_write_dword_bounded()
   / flash_erase_page_bounded(), whose BSY waits are bounded by the DWT cycle
   counter rather than by HAL_GetTick() (the tick is frozen in an NMI because
   SysTick cannot preempt it). sram2_flash_prepare() adds the missing bound at
   the entry of the path - and sanitises FLASH->CR, because a task preempted
   mid-sequence leaves operation-select bits armed that would turn this write
   into a programming-sequence error.

   Note on the watchdog: the IWDG *is* enabled in this build since W2
   (Core/Src/hw_watchdog.c), so a wedged Flash controller would eventually be
   ended by a watchdog reset. That is a mission-degrading outcome, not
   containment, so the cycle-counted bound stays the primary mechanism: the
   handler gives up in ~52 ms and still reaches its NVIC_SystemReset() with
   the evidence latched, instead of burning the full watchdog period.

   A record that cannot be written is counted in the reset-stable store, so
   the loss is observable from the ground instead of silent - and the store
   itself already carries the fault flag, so the safe state on the next boot
   never depends on this Flash write succeeding. */
static void sram2_persist(const sram2_parity_record_t *rec, uint8_t trigger)
{
    laststates_entry_t entry;

    sram2_fill_entry(&entry, rec, trigger);

    if (sram2_flash_prepare() != 0) {
        sram2_store_note_dropped();
        return;
    }
    if (laststates_write(&entry) != 0) {
        sram2_store_note_dropped();
    }
}

/* ---------- Public API ---------- */

void sram2_parity_init(void)
{
    sram2_parity_record_t rec;
    int erase_rc;
    int erase_stuck = 0;
    /* Set as soon as anything more severe than "the option byte is off" has
       been latched into last_event during this boot, so the bookkeeping note
       at the very end cannot stomp it (Kilo #26, comment id 3741110983). */
    int event_latched = 0;

    /* SYSCFG carries both the parity flag and the SRAM2 erase control. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    /* The reset-stable store is the first thing to establish: everything
       below reads or records into it. */
    sram2_store_validate();

    /* Carry the parity-reset history into this run's event counter so the
       records written from now on show how many times SRAM2 has bitten,
       instead of a permanent 1 from a counter that every reset zeroes. */
    sram2_parity_events = sram2_store.parity_resets;

    /* A parity NMI in the previous run left its evidence here: SYSCFG_CFGR2
       .SPF is cleared by the handler (and by the reset itself), so it cannot
       be the carrier.

       READ the flag, do NOT consume it (Kilo #23, comment id 3740885202).
       Between this point and the state machine actually acting on it sit
       dual_bank_init() (can OB_Launch and never return) and
       boot_crc_apply_policy() (resets up to BOOT_CRC_MAX_RESET_ATTEMPTS
       times), plus any fault handler. Clearing the store here would hand the
       only surviving copy of the finding to a .bss flag that the next reset
       zeroes, and the boot after that would stroll into READY. The store is
       released by sram2_parity_boot_fault_ack(), which the state machine
       calls once the OBSW has actually reached the safe state. */
    if (sram2_store.boot_fault != 0U) {
        sram2_boot_fault_flag = 1U;
        /* The previous run latched a finding (typically a real parity NMI).
           Its last_event is the interesting one; nothing below may stomp it. */
        event_latched = 1;
    }

    /* A flag latched before this point means SRAM2 reported a parity error in
       the previous run (or during the reset sequence). Record it, then clear -
       an already-set SPF would otherwise mask the next genuine event.

       Ordering note (Kilo #23, comment id 3740885207): the record is built
       here, while the status registers still hold the failure context, and
       written later by sram2_parity_persist_boot_records(). The reason is
       ordering of the forensic trail, NOT a write hazard: laststates_write()
       re-derives the cursor through laststates_resync() whenever the target
       slot is not erased (App/memory/memory.c), so writing now would neither
       fail with PGSERR nor overwrite anything - it would simply land ahead of
       the boot bookkeeping laststates_init() has yet to establish, leaving
       ground to read the trail out of order. The earlier comment claimed a
       PGSERR apocalypse; that was fiction. */
    if (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_PE) != 0U) {
        sram2_parity_events++;
        sram2_fill_record(&rec, SRAM2_EVENT_BOOT_LATCH);
        sram2_queue_boot_record(&rec);
        sram2_store_note_event(SRAM2_EVENT_BOOT_LATCH);
        sram2_boot_fault_flag = 1U;
        event_latched = 1;
        __HAL_SYSCFG_CLEAR_FLAG();
    }

    /* Give every SRAM2 byte a defined value and a valid parity bit before any
       critical object is touched. Without this, reads of never-written cells
       raise spurious parity NMIs when the check is enabled. */
    erase_rc = sram2_hw_erase();

    if (erase_rc == SRAM2_ERASE_BUSY) {
        /* Acknowledged but still running. Give the engine a second, longer
           bounded window rather than assuming the worst. */
        if (sram2_erase_settle_wait() == 0) {
            erase_rc = SRAM2_ERASE_OK;   /* late, but complete */
        }
    }

    if (erase_rc == SRAM2_ERASE_BUSY) {
        /* Still under erase after both bounds (Kilo #23, comment id
           3740885200). NOTHING may be written into SRAM2 here - not the
           software fill, not the .sram2 init image: the erase would finish
           afterwards and zero it all again with perfectly valid parity, so
           the parity check would stay silent and the OBSW would run on an
           all-zero critical state that looks pristine. Record and reboot
           instead.

           The reboot budget is bounded and lives in the reset-stable store,
           so a permanently wedged SYSCFG cannot turn the spacecraft into a
           reset loop: once it is spent, boot continues with SRAM2 left
           untouched and the boot-fault latch takes the OBSW to STATE_CRIT,
           where ground can act. */
        sram2_parity_events++;
        sram2_fill_record(&rec, SRAM2_EVENT_ERASE_BUSY);

        if (sram2_store.erase_busy_resets < SRAM2_ERASE_BUSY_RESET_LIMIT) {
            /* Stashes SYSCFG->SCSR / CFGR2 of the FIRST occurrence, which the
               reset below would otherwise destroy along with rec and the
               pending queue (Kilo #26, comment id 3741110980). */
            sram2_store_note_erase_busy(rec.scsr, rec.cfgr2);
            NVIC_SystemReset();
            for (;;) {
                /* NVIC_SystemReset() does not return. */
            }
        }

        /* Budget spent: continue into STATE_CRIT instead of looping forever.
           note_boot_fault(), NOT note_fault(): no parity error occurred and no
           reset is being taken, so bumping parity_resets here would pad the
           only "one upset vs a dead cell" discriminator ground has - and
           sram2_parity_events is reseeded from it on every later boot, which
           compounds the error (Kilo #26, comment id 3741110980). */
        sram2_store_note_boot_fault(SRAM2_EVENT_ERASE_BUSY);
        event_latched = 1;

        /* Replay the first occurrence's register context ahead of this one:
           it is the state before the reset loop started, i.e. the one that
           says why the engine wedged. Queued first because
           sram2_queue_boot_record() keeps the oldest when the queue is full. */
        if ((sram2_store.first_busy_scsr != 0U) ||
            (sram2_store.first_busy_cfgr2 != 0U)) {
            sram2_parity_record_t first = rec;
            first.scsr  = sram2_store.first_busy_scsr;
            first.cfgr2 = sram2_store.first_busy_cfgr2;
            sram2_queue_boot_record(&first);
        }
        sram2_queue_boot_record(&rec);
        sram2_boot_fault_flag = 1U;
        erase_stuck = 1;

        /* Latch the block as UNUSABLE for the rest of this boot. Skipping only
           sram2_copy_init_image() was not enough: state_machine_init() runs a
           few functions later and reads obsw_state.magic out of SRAM2 and
           writes current_state straight back into it. With SRAM2_PE actually
           programmed that read raises a parity NMI -> reset -> the budget is
           already spent -> read again -> NMI, forever; and without it the
           erase completes afterwards and zeroes current_state to STATE_OFF
           with valid parity and a matching SEU shadow, i.e. the all-zero
           critical state that looks pristine, arriving one function later
           (Kilo #26, comment id 3741110981). Every .sram2 consumer must now
           gate on sram2_parity_region_usable() and run from its SRAM1
           defaults; the boot-fault latch keeps the OBSW in STATE_CRIT. */
        sram2_region_usable = 0U;
    } else if (erase_rc == SRAM2_ERASE_NOT_ACKED) {
        /* The hardware never took the request, so the block is idle and
           untouched: defining every byte by hand is safe here (a store writes
           the parity bits too) and necessary, because the alternative is
           leaving undefined parity for the first read to trip over. */
        sram2_sw_fill();
    }

    if (erase_stuck == 0) {
        sram2_copy_init_image();
    }

    if ((erase_rc != SRAM2_ERASE_OK) && (erase_stuck == 0)) {
        sram2_parity_events++;
        sram2_fill_record(&rec, SRAM2_EVENT_ERASE_FAIL);
        sram2_queue_boot_record(&rec);
        sram2_store_note_event(SRAM2_EVENT_ERASE_FAIL);
        sram2_boot_fault_flag = 1U;
        event_latched = 1;
    }

    /* The erase itself must not leave a stale flag behind. */
    __HAL_SYSCFG_CLEAR_FLAG();

#if defined(SRAM2_PARITY_BREAK_TIM_LOCK) && (SRAM2_PARITY_BREAK_TIM_LOCK == 1)
    /* Route the parity error to the TIM1/8/15/16/17 break inputs so PWM
       actuator outputs are forced to their safe state in hardware. Locked
       until the next system reset. */
    __HAL_SYSCFG_BREAK_SRAM2PARITY_LOCK();
#endif

#if defined(SRAM2_PARITY_PROGRAM_OPTION_BYTE) && (SRAM2_PARITY_PROGRAM_OPTION_BYTE == 1)
    if ((FLASH->OPTR & FLASH_OPTR_SRAM2_PE) != 0U) {
        /* Does not return on success: launching the option bytes resets. */
        (void)sram2_parity_program_option_byte();
    }
#endif

    /* SRAM2_PE is active low: bit clear == parity check enabled. */
    sram2_status = ((FLASH->OPTR & FLASH_OPTR_SRAM2_PE) == 0U)
                       ? SRAM2_PARITY_STATUS_ENABLED
                       : SRAM2_PARITY_STATUS_DISABLED;

    /* The hardware check is a non-volatile option-byte setting, so a build
       that never programs it silently degrades this module to bookkeeping:
       the NMI it is built around can never fire.

       The primary carrier for that condition is sram2_status, published
       through sram2_parity_get_status() / sram2_parity_is_enabled() - it is a
       status, not an event. Writing it into last_event UNCONDITIONALLY made
       this the last store write of every boot, stomping BOOT_LATCH,
       ERASE_BUSY, ERASE_FAIL and any PARITY_NMI carried over from the
       previous run; and because SRAM2_PARITY_PROGRAM_OPTION_BYTE is not in the
       Makefile C_DEFS, on an unprogrammed part it fired on EVERY boot, so
       last_event was permanently 5 - a constant, not telemetry - which then
       poisoned the corroboration check in sram2_store_recover() on the next
       boot (Kilo #26, comment id 3741110983).

       So it is recorded only when nothing more severe is already latched for
       this boot. */
    if ((sram2_status != SRAM2_PARITY_STATUS_ENABLED) && (event_latched == 0)) {
        sram2_store_note_event(SRAM2_EVENT_PARITY_DISABLED);
    }
}

int sram2_parity_persist_boot_records(void)
{
    int written = 0;
    int failed  = 0;

    for (uint32_t i = 0U; i < sram2_pending_count; i++) {
        if (laststates_write(&sram2_pending[i]) == 0) {
            written++;
        } else {
            failed = 1;
            sram2_store_note_dropped();
        }
    }

    /* One shot: never retried, so a wedged Flash cannot turn boot into a write
       storm. sram2_boot_fault_flag is deliberately left set - the safe-state
       decision must not depend on the record having reached Flash. */
    sram2_pending_count = 0U;

    return (failed != 0) ? -1 : written;
}

int sram2_parity_boot_fault(void)
{
    return (sram2_boot_fault_flag != 0U) ? 1 : 0;
}

void sram2_parity_boot_fault_ack(void)
{
    sram2_store_validate();
    sram2_store.boot_fault = 0U;
    sram2_store_seal();

    /* sram2_boot_fault_flag is deliberately NOT cleared: it is this run's
       record of how boot went, and the state machine's own confinement latch
       is derived from it. Only the reset-stable half is released. */
}

uint32_t sram2_parity_reset_count(void)
{
    return (sram2_store_is_valid() != 0) ? sram2_store.parity_resets : 0U;
}

uint32_t sram2_parity_erase_busy_resets(void)
{
    return (sram2_store_is_valid() != 0) ? sram2_store.erase_busy_resets : 0U;
}

uint32_t sram2_parity_last_event(void)
{
    return (sram2_store_is_valid() != 0) ? sram2_store.last_event
                                         : (uint32_t)SRAM2_EVENT_STORE_LOST;
}

uint32_t sram2_parity_dropped_records(void)
{
    return (sram2_store_is_valid() != 0) ? sram2_store.dropped_records : 0U;
}

sram2_parity_status_t sram2_parity_get_status(void)
{
    return sram2_status;
}

int sram2_parity_is_enabled(void)
{
    return (sram2_status == SRAM2_PARITY_STATUS_ENABLED) ? 1 : 0;
}

uint32_t sram2_parity_error_count(void)
{
    return sram2_parity_events;
}

int sram2_parity_region_usable(void)
{
    return (sram2_region_usable != 0U) ? 1 : 0;
}

int sram2_restore_from_image(void *obj, size_t len)
{
    const uint8_t *ram_start = (const uint8_t *)&_ssram2;
    const uint8_t *ram_end   = (const uint8_t *)&_esram2;
    const uint8_t *img_start = (const uint8_t *)&_sisram2;
    const uint8_t *dst = (const uint8_t *)obj;
    uintptr_t dst_end;

    if ((obj == NULL) || (len == 0U)) {
        return -1;
    }
    /* Refuse while the block is under a wedged erase engine: this function
       WRITES into .sram2, and every byte it writes is zeroed again - with
       valid parity - when the erase finally completes, which is precisely the
       silent-corruption case the caller thinks it is repairing (Kilo #26,
       comment id 3741110981). Gating here covers every consumer at once. */
    if (sram2_region_usable == 0U) {
        return -1;
    }
    /* Overflow-safe bounds check: dst+len must not wrap the address space or
       run past the end of the initialised .sram2 section. A flipped len could
       otherwise make (dst + len) wrap to a small value and pass the check.

       Both sides of every compare are uintptr_t. Mixing a uintptr_t with a
       const uint8_t * in an ORDERED compare is a "comparison between pointer
       and integer" - a warning on gcc 12, a hard error on the gcc 15.2
       toolchain the container builds with. */
    if (__builtin_add_overflow((uintptr_t)dst, (uintptr_t)len, &dst_end) ||
        ((uintptr_t)dst < (uintptr_t)ram_start) ||
        (dst_end > (uintptr_t)ram_end)) {
        return -1;   /* not an object of the initialised .sram2 section */
    }

    memcpy((void *)dst, img_start + (size_t)(dst - ram_start), len);
    __DSB();
    return 0;
}

int sram2_section_contains(const void *obj)
{
    const uint8_t *p    = (const uint8_t *)obj;
    const uint8_t *low  = (const uint8_t *)&_ssram2;
    const uint8_t *high = (const uint8_t *)&_esram2;

    /* True only for objects placed in the initialised .sram2 section (those
       with a Flash load image). Objects in .sram2_noinit are excluded. */
    return ((p >= low) && (p < high)) ? 1 : 0;
}

void sram2_parity_nmi_handler(void)
{
    sram2_parity_record_t rec;   /* on the handler stack, i.e. in SRAM1 */
    uint32_t event_id = SRAM2_EVENT_OTHER_NMI;

    if (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_PE) != 0U) {
        event_id = SRAM2_EVENT_PARITY_NMI;
    }

    sram2_parity_events++;
    sram2_fill_record(&rec, event_id);

    /* Evidence first, into the sink that can neither fail nor block: the
       reset-stable store in SRAM1. A parity NMI is the one event the next
       boot has to know about, and SPF - the only other trace - is cleared
       immediately below (and by the reset in any case), so latching it here
       is what makes "parity NMI -> record -> reset -> safe state" real
       instead of a comment. */
    if (event_id == SRAM2_EVENT_PARITY_NMI) {
        sram2_store_note_fault(event_id);
    } else {
        /* Another NMI source (RCC CSS): recorded, but it says nothing about
           the integrity of SRAM2, so it must not force the safe state. */
        sram2_store_note_event(event_id);
    }

    /* Clear the sources so the record above is the only surviving evidence and
       a re-entrant NMI cannot storm before the reset takes effect. */
    if (event_id == SRAM2_EVENT_PARITY_NMI) {
        __HAL_SYSCFG_CLEAR_FLAG();
    }
    if (__HAL_RCC_GET_IT(RCC_IT_CSS) != RESET) {
        /* Belt and braces: a CSS event arriving in the same NMI as a parity
           error. The CSS-only case is routed to
           sram2_clock_fault_nmi_handler() by NMI_Handler() and never gets
           here (Kilo #26, comment id 3741110986). */
        __HAL_RCC_CLEAR_IT(RCC_IT_CSS);
    }

    sram2_persist(&rec, TRIGGER_SRAM2_PARITY);

    NVIC_SystemReset();

    for (;;) {
        /* NVIC_SystemReset() does not return; guard against a failed reset. */
    }
}

void sram2_clock_fault_nmi_handler(void)
{
    sram2_parity_record_t rec;   /* on the handler stack, i.e. in SRAM1 */
    uint32_t seen;

    /* Deliberately does NOT touch sram2_parity_events, parity_resets or the
       boot-fault latch: a dead HSE crystal says nothing about the integrity
       of SRAM2. Counting it there filed the record under TRIGGER_SRAM2_PARITY
       with an inflated RAM-health counter and had ground diagnosing memory
       degradation for an oscillator failure (Kilo #26, id 3741110986). */
    sram2_fill_record(&rec, SRAM2_EVENT_CLOCK_NMI);

    sram2_store_validate();
    seen = sram2_store.clock_faults;
    if (seen < SRAM2_STORE_COUNT_SANE) {
        sram2_store.clock_faults = seen + 1U;
    }
    sram2_store.last_event = (uint32_t)SRAM2_EVENT_CLOCK_NMI;
    sram2_store_seal();

    /* Clear CSSF before the record is written: the NMI is driven by the flag,
       so leaving it set would re-enter this handler on every instruction
       boundary and never reach the Flash write. */
    if (__HAL_RCC_GET_IT(RCC_IT_CSS) != RESET) {
        __HAL_RCC_CLEAR_IT(RCC_IT_CSS);
    }

    /* Bounded record budget: an HSE failure is permanent, so an unbounded
       path would program the whole LastStates pool away and destroy the very
       trail it is meant to preserve. The counter keeps growing in the
       reset-stable store, so the loss stays observable. */
    if (seen < SRAM2_CLOCK_FAULT_RECORD_LIMIT) {
        sram2_persist(&rec, TRIGGER_CLOCK_FAULT);
    }

    /* RETURNS. The hardware has already switched the system clock to the HSI16
       fallback (RM0351 rev 9, RCC clock security system), and no reset can
       resurrect a crystal - NVIC_SystemReset() here would only be an endless
       reboot loop on a spacecraft that is otherwise perfectly able to run. */
}

#if defined(SRAM2_PARITY_PROGRAM_OPTION_BYTE) && (SRAM2_PARITY_PROGRAM_OPTION_BYTE == 1)
int sram2_parity_program_option_byte(void)
{
    FLASH_OBProgramInitTypeDef ob;

    memset(&ob, 0, sizeof(ob));
    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType   = OB_USER_SRAM2_PE | OB_USER_SRAM2_RST;
    /* OB_SRAM2_PARITY_ENABLE == 0, OB_SRAM2_RST_ERASE == 0 (both active low). */
    ob.USERConfig = OB_SRAM2_PARITY_ENABLE | OB_SRAM2_RST_ERASE;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return -1;
    }
    if (HAL_FLASH_OB_Unlock() != HAL_OK) {
        (void)HAL_FLASH_Lock();
        return -1;
    }
    if (HAL_FLASHEx_OBProgram(&ob) != HAL_OK) {
        (void)HAL_FLASH_OB_Lock();
        (void)HAL_FLASH_Lock();
        return -1;
    }

    /* Loading the new option bytes triggers a system reset: never returns. */
    (void)HAL_FLASH_OB_Launch();

    (void)HAL_FLASH_OB_Lock();
    (void)HAL_FLASH_Lock();
    return 0;
}
#endif
