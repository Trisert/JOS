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

   Validity is a magic plus its complement: a reset that catches the struct
   half-written leaves the pair inconsistent, and the store is re-initialised
   instead of being believed. */
#define SRAM2_STORE_MAGIC       0x53324653U   /* "S2FS" */

typedef struct {
    uint32_t magic;             /* SRAM2_STORE_MAGIC                        */
    uint32_t magic_inv;         /* ~SRAM2_STORE_MAGIC                       */
    uint32_t boot_fault;        /* 1: last run ended on a parity finding    */
    uint32_t last_event;        /* SRAM2_EVENT_* of that finding            */
    uint32_t parity_resets;     /* parity-triggered resets since power-on   */
    uint32_t dropped_records;   /* LastStates writes the fault path lost    */
} sram2_fault_store_t;

static sram2_fault_store_t sram2_store __attribute__((section(".noinit"), used));

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

/* ---------- Helpers ---------- */

static int sram2_store_is_valid(void)
{
    return ((sram2_store.magic == SRAM2_STORE_MAGIC) &&
            (sram2_store.magic_inv == (uint32_t)~SRAM2_STORE_MAGIC)) ? 1 : 0;
}

static void sram2_store_reset(void)
{
    memset(&sram2_store, 0, sizeof(sram2_store));
    sram2_store.magic     = SRAM2_STORE_MAGIC;
    sram2_store.magic_inv = (uint32_t)~SRAM2_STORE_MAGIC;
    __DSB();
}

/* Note the event without claiming the next boot: used for findings that are
   already handled inside the boot they are detected in. */
static void sram2_store_note_event(uint32_t event_id)
{
    if (sram2_store_is_valid() == 0) {
        sram2_store_reset();
    }
    sram2_store.last_event = event_id;
    __DSB();
}

/* Latch a parity fault for the *next* boot. Callable from the NMI: no HAL, no
   lock, no Flash - it can neither fail nor block, which is what makes it a
   usable sink when the LastStates write cannot happen. */
static void sram2_store_note_fault(uint32_t event_id)
{
    if (sram2_store_is_valid() == 0) {
        sram2_store_reset();
    }
    sram2_store.boot_fault = 1U;
    sram2_store.last_event = event_id;
    sram2_store.parity_resets++;
    __DSB();
}

static void sram2_store_note_dropped(void)
{
    if (sram2_store_is_valid() == 0) {
        sram2_store_reset();
    }
    sram2_store.dropped_records++;
    __DSB();
}

/* @retval 0 the erase was accepted by the hardware and ran to completion,
   @retval -1 it was never acknowledged, or it did not finish in time. */
static int sram2_hw_erase(void)
{
    const uint32_t busy_mask = SYSCFG_SCSR_SRAM2ER | SYSCFG_SCSR_SRAM2BSY;
    int started = 0;
    int done    = 0;

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

    __DSB();
    return ((started != 0) && (done != 0)) ? 0 : -1;
}

/* Software fallback for an erase that was never acknowledged or never
   finished. A store also writes the parity bits of the bytes it covers, so
   filling the whole RAM2 region by hand leaves every byte with a defined
   value and valid parity - slower than the hardware erase (16 K stores) but
   it removes the undefined-parity state instead of leaving it for the first
   read to trip over. Without it the failure path is circular: detecting the
   problem later means *reading* SRAM2, and that read is precisely what raises
   the parity NMI this module exists to prevent. */
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
   be written to Flash later without losing information. */
static void sram2_fill_entry(laststates_entry_t *entry,
                             const sram2_parity_record_t *rec)
{
    memset(entry, 0, sizeof(*entry));
    entry->timestamp  = HAL_GetTick();
    entry->state_from = SRAM2_STATE_UNKNOWN;
    entry->state_to   = SRAM2_STATE_UNKNOWN;
    entry->trigger    = TRIGGER_SRAM2_PARITY;
    memcpy(entry->context, rec, sizeof(*rec));
}

/* Queue a boot-time finding for persistence after laststates_init(). */
static void sram2_queue_boot_record(const sram2_parity_record_t *rec)
{
    if (sram2_pending_count >= SRAM2_PENDING_MAX) {
        return;   /* keep the oldest: it carries the first cause */
    }
    sram2_fill_entry(&sram2_pending[sram2_pending_count], rec);
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

/* @retval 0 the Flash controller is idle, -1 it stayed busy for longer than
   SRAM2_FLASH_IDLE_TIMEOUT_CYCLES. */
static int sram2_flash_idle_wait(void)
{
    uint32_t start;

    sram2_dwt_enable();
    start = DWT->CYCCNT;

    while (__HAL_FLASH_GET_FLAG(FLASH_FLAG_BSY) != RESET) {
        if ((DWT->CYCCNT - start) >= SRAM2_FLASH_IDLE_TIMEOUT_CYCLES) {
            return -1;
        }
    }
    return 0;
}

/* Serialise the record into the LastStates pool (internal Flash).

   Bounded by construction, which matters because this also runs in the NMI:
   App/memory/memory.c programs and erases through flash_write_dword_bounded()
   / flash_erase_page_bounded(), whose BSY waits are bounded by the DWT cycle
   counter rather than by HAL_GetTick() (the tick is frozen in an NMI because
   SysTick cannot preempt it). The wait below adds the missing bound at the
   entry of the path: if a task was already programming Flash when the NMI
   hit, the handler waits a fixed number of CPU cycles for the controller and
   then gives up instead of queueing behind an operation that may never end.

   The previous comment here claimed a wedged Flash controller was "covered by
   the independent watchdog". That was wrong: IWDG is not enabled in this
   build (HAL_IWDG_MODULE_ENABLED is commented out in stm32l4xx_hal_conf.h and
   there is no MX_IWDG_Init()), so nothing would have ended such a hang. The
   containment is the cycle-counted bound, not a watchdog.

   A record that cannot be written is counted in the reset-stable store, so
   the loss is observable from the ground instead of silent - and the store
   itself already carries the fault flag, so the safe state on the next boot
   never depends on this Flash write succeeding. */
static void sram2_persist(const sram2_parity_record_t *rec)
{
    laststates_entry_t entry;

    sram2_fill_entry(&entry, rec);

    if (sram2_flash_idle_wait() != 0) {
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
    int erase_failed;

    /* SYSCFG carries both the parity flag and the SRAM2 erase control. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    /* The reset-stable store is the first thing to establish: everything
       below reads or records into it. An invalid magic pair means a
       power-on / brown-out reset (or a corrupted struct), i.e. no history. */
    if (sram2_store_is_valid() == 0) {
        sram2_store_reset();
    }

    /* Carry the parity-reset history into this run's event counter so the
       records written from now on show how many times SRAM2 has bitten,
       instead of a permanent 1 from a counter that every reset zeroes. */
    sram2_parity_events = sram2_store.parity_resets;

    /* A parity NMI in the previous run left its evidence here: SYSCFG_CFGR2.SPF
       is cleared by the handler (and by the reset itself), so it cannot be the
       carrier. Consume the flag - it becomes this boot's fault state, and a
       later clean boot must not inherit it - while the reset counter stays. */
    if (sram2_store.boot_fault != 0U) {
        sram2_boot_fault_flag  = 1U;
        sram2_store.boot_fault = 0U;
        __DSB();
    }

    /* A flag latched before this point means SRAM2 reported a parity error in
       the previous run (or during the reset sequence). Record it, then clear -
       an already-set SPF would otherwise mask the next genuine event.
       Ordering note: this function must run before every other init (it
       hardware-erases SRAM2), i.e. before laststates_init(), which resets the
       pool write index. Persisting here would therefore place the record at an
       index the first post-boot transition overwrites - and on a pool that
       already holds an entry at index 0 the double-word program would fail
       with PGSERR. The record is built now, while the status registers still
       hold the failure context, and written by
       sram2_parity_persist_boot_records() once the pool is initialised. */
    if (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_PE) != 0U) {
        sram2_parity_events++;
        sram2_fill_record(&rec, SRAM2_EVENT_BOOT_LATCH);
        sram2_queue_boot_record(&rec);
        sram2_store_note_event(SRAM2_EVENT_BOOT_LATCH);
        sram2_boot_fault_flag = 1U;
        __HAL_SYSCFG_CLEAR_FLAG();
    }

    /* Give every SRAM2 byte a defined value and a valid parity bit before any
       critical object is touched. Without this, reads of never-written cells
       raise spurious parity NMIs when the check is enabled. */
    erase_failed = (sram2_hw_erase() != 0) ? 1 : 0;
    if (erase_failed != 0) {
        /* The hardware never acknowledged or never finished the erase, so
           part of the block may still carry undefined parity. Define it in
           software (a store writes the parity bits too) so the block is safe
           to read, then record the finding: the boot-fault flag makes the
           OBSW come up in the safe state instead of continuing as nominal. */
        sram2_sw_fill();
    }

    sram2_copy_init_image();

    if (erase_failed != 0) {
        sram2_parity_events++;
        sram2_fill_record(&rec, SRAM2_EVENT_ERASE_FAIL);
        sram2_queue_boot_record(&rec);
        sram2_store_note_event(SRAM2_EVENT_ERASE_FAIL);
        sram2_boot_fault_flag = 1U;
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

uint32_t sram2_parity_reset_count(void)
{
    return (sram2_store_is_valid() != 0) ? sram2_store.parity_resets : 0U;
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
    /* Overflow-safe bounds check: dst+len must not wrap the address space or
       run past the end of the initialised .sram2 section. A flipped len could
       otherwise make (dst + len) wrap to a small value and pass the check. */
    if (__builtin_add_overflow((uintptr_t)dst, (uintptr_t)len, &dst_end) ||
        (dst < ram_start) || (dst_end > ram_end)) {
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
        /* Clock security system: HSE failure also vectors to the NMI. */
        __HAL_RCC_CLEAR_IT(RCC_IT_CSS);
    }

    sram2_persist(&rec);

    NVIC_SystemReset();

    for (;;) {
        /* NVIC_SystemReset() does not return; guard against a failed reset. */
    }
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
