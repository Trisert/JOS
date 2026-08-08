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

/* Bounded wait for the SRAM2 hardware erase. The erase of 64 KB takes a few
   hundred microseconds; the loop is a fixed iteration count rather than a
   HAL_GetTick() timeout because init() runs before the RTOS and, on the NMI
   path, SysTick cannot preempt the handler. */
#define SRAM2_ERASE_POLL_LIMIT  1000000UL

/* Number of boot-time findings that can be queued for deferred persistence.
   Two is enough for the worst case (parity flag latched at boot + the erase
   that follows it failing). */
#define SRAM2_PENDING_MAX       2U

/* ---------- Module state ----------
   Deliberately in SRAM1 (default .bss / .data): memory that has just reported
   a parity error must not be trusted to hold its own diagnostics. */
static volatile uint32_t sram2_parity_events = 0U;
static sram2_parity_status_t sram2_status = SRAM2_PARITY_STATUS_UNKNOWN;

/* Boot-time records waiting for the LastStates pool to be initialised, and the
   flag that makes the boot fault visible to the state machine. */
static laststates_entry_t sram2_pending[SRAM2_PENDING_MAX];
static uint32_t sram2_pending_count = 0U;
static uint8_t  sram2_boot_fault_flag = 0U;

/* ---------- Helpers ---------- */

/* @retval 0 the erase completed, -1 it did not finish within the poll limit. */
static int sram2_hw_erase(void)
{
    int done = 0;

    /* Unlock the write protection of SCSR.SRAM2ER (key sequence 0xCA, 0x53),
       then start the erase. The hardware writes 0x00 with correct parity to
       every byte of SRAM2. */
    __HAL_SYSCFG_SRAM2_WRP_UNLOCK();
    __HAL_SYSCFG_SRAM2_ERASE();

    /* Poll SCSR.SRAM2ER, which hardware clears when the erase has finished.
       SRAM2BSY alone is not sufficient: the APB write that starts the erase is
       posted, so BSY can still read 0 on the first poll and the loop would
       exit before the erase even began. BSY is checked as well so the function
       only reports success once the block is idle. */
    for (uint32_t i = 0U; i < SRAM2_ERASE_POLL_LIMIT; i++) {
        if (((SYSCFG->SCSR & SYSCFG_SCSR_SRAM2ER) == 0U) &&
            (__HAL_SYSCFG_GET_FLAG(SYSCFG_FLAG_SRAM2_BUSY) == 0U)) {
            done = 1;
            break;
        }
    }

    /* Symmetry with the unlock above: writing any value that is not the
       0xCA / 0x53 sequence reactivates the write protection of SRAM2ER
       (RM0351 rev 9, SYSCFG_SKR), so no stray write can wipe the parity-
       protected block once the OBSW is running. */
    SYSCFG->SKR = 0x00U;

    __DSB();
    return (done != 0) ? 0 : -1;
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

/* Serialise the record into the LastStates pool (internal Flash).
   Note: HAL_FLASH_Program() polls with HAL_GetTick()-based timeouts and
   SysTick cannot preempt an NMI, so a HAL timeout cannot expire on the NMI
   path. There is no independent watchdog yet (IWDG is not initialised in this
   build), so a Flash controller that never clears BSY would hang the handler;
   bounding that wait is tracked as a follow-up and must be closed before
   flight. */
static void sram2_persist(const sram2_parity_record_t *rec)
{
    laststates_entry_t entry;

    sram2_fill_entry(&entry, rec);

    /* Best effort: if the Flash write fails there is no alternative sink and
       the reset must happen regardless (the reset is the containment action). */
    (void)laststates_write(&entry);
}

/* Force the PWM actuator outputs to their inactive level without touching a
   HAL handle, a mutex or the clock tree - safe to call from an NMI. Clearing
   MOE releases the TIM1 outputs to their idle state and CEN stops the counter,
   so no actuator is left driven while the reset propagates. */
static void sram2_force_actuators_safe(void)
{
    if (__HAL_RCC_TIM1_IS_CLK_ENABLED()) {
        TIM1->BDTR &= ~(TIM_BDTR_MOE);
        TIM1->CR1  &= ~(TIM_CR1_CEN);
    }
    __DSB();
}

/* ---------- Public API ---------- */

void sram2_parity_init(void)
{
    sram2_parity_record_t rec;

    /* SYSCFG carries both the parity flag and the SRAM2 erase control. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();

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
        sram2_boot_fault_flag = 1U;
        __HAL_SYSCFG_CLEAR_FLAG();
    }

    /* Give every SRAM2 byte a defined value and a valid parity bit before any
       critical object is touched. Without this, reads of never-written cells
       raise spurious parity NMIs when the check is enabled. */
    if (sram2_hw_erase() == 0) {
        sram2_copy_init_image();
    } else {
        /* The erase never reported completion, so part of SRAM2 may still
           carry undefined parity. Copying the image on top of a half-erased
           block would hide that, so the copy is skipped: state_machine_init()
           finds a wrong magic and restores its critical structure from the
           Flash load image (which also writes valid parity for those bytes).
           The finding is recorded and the boot fault flag makes the OBSW come
           up in the safe state instead of continuing as if nominal. */
        sram2_fill_record(&rec, SRAM2_EVENT_ERASE_FAIL);
        sram2_queue_boot_record(&rec);
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
    const uintptr_t ram_start = (uintptr_t)&_ssram2;
    const uintptr_t ram_end   = (uintptr_t)&_esram2;
    const uint8_t  *img_start = (const uint8_t *)&_sisram2;
    const uintptr_t dst_addr  = (uintptr_t)obj;

    if ((obj == NULL) || (len == 0U)) {
        return -1;
    }
    /* Range check without ever forming an out-of-range pointer: comparing
       addresses and remaining length cannot overflow. */
    if ((dst_addr < ram_start) || (dst_addr >= ram_end)) {
        return -1;   /* not an object of the initialised .sram2 section */
    }
    if ((size_t)(ram_end - dst_addr) < len) {
        return -1;   /* object would run past the end of .sram2 */
    }

    memcpy(obj, img_start + (size_t)(dst_addr - ram_start), len);
    __DSB();
    return 0;
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

    /* Clear the sources so the record above is the only surviving evidence and
       a re-entrant NMI cannot storm before the reset takes effect. */
    if (event_id == SRAM2_EVENT_PARITY_NMI) {
        __HAL_SYSCFG_CLEAR_FLAG();
    }
    if (__HAL_RCC_GET_IT(RCC_IT_CSS) != RESET) {
        /* Clock security system: HSE failure also vectors to the NMI. */
        __HAL_RCC_CLEAR_IT(RCC_IT_CSS);
    }

    /* Containment before evidence: put the actuators in their safe state so a
       corrupted control word cannot keep driving them while the record is
       written and the reset propagates. */
    sram2_force_actuators_safe();

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
