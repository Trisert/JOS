/**
  ******************************************************************************
  * @file    sram2_parity.h
  * @brief   SRAM2 hardware-parity protection for critical OBSW data (W2-3).
  *
  * The STM32L496VGTx carries a 64 KB SRAM2 block at 0x10000000 that is covered
  * by a hardware parity bit per byte. A parity error (a single-event upset in
  * a protected word, for instance) raises a non-maskable interrupt, which this
  * module turns into a recorded, deterministic reboot instead of silent data
  * corruption.
  *
  * Placement macros:
  *   SRAM2_CRITICAL        - initialised critical data. The .sram2 section is
  *                           loaded into Flash and copied into SRAM2 by
  *                           sram2_parity_init(); static initialisers work.
  *   SRAM2_CRITICAL_NOINIT - buffers with no meaningful initial value. The
  *                           .sram2_noinit section is NOLOAD and is zeroed by
  *                           the SRAM2 hardware erase in sram2_parity_init(),
  *                           so it costs no Flash.
  *
  * Standards: NASA-STD-8739.8 (data integrity, no silent failure),
  *            ECSS-E-ST-40C / ECSS-Q-ST-80C (fault detection and recording).
  ******************************************************************************
  */

#ifndef SRAM2_PARITY_H
#define SRAM2_PARITY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Placement attributes for critical runtime data ---------- */
/* "used" keeps the object out of reach of -fdata-sections + --gc-sections. */
#define SRAM2_CRITICAL          __attribute__((section(".sram2"), used))
#define SRAM2_CRITICAL_NOINIT   __attribute__((section(".sram2_noinit"), used))

/* ---------- Parity subsystem status ---------- */
typedef enum {
    SRAM2_PARITY_STATUS_UNKNOWN  = 0,  /* sram2_parity_init() not run yet     */
    SRAM2_PARITY_STATUS_ENABLED  = 1,  /* option byte SRAM2_PE programmed     */
    SRAM2_PARITY_STATUS_DISABLED = 2,  /* parity check off in the option byte */
} sram2_parity_status_t;

/* ---------- Event ids recorded in the LastStates pool ---------- */
enum {
    SRAM2_EVENT_PARITY_NMI = 0U,  /* NMI with SYSCFG_CFGR2.SPF set           */
    SRAM2_EVENT_OTHER_NMI  = 1U,  /* NMI from another source (e.g. RCC CSS)  */
    SRAM2_EVENT_BOOT_LATCH = 2U,  /* SPF already latched when init() ran     */
};

/* Marker so ground can find these records inside a LastStates context blob. */
#define SRAM2_PARITY_RECORD_MAGIC   0x53503245U   /* "SP2E" */

/**
  * @brief Post-mortem record persisted on a parity NMI.
  * @note  Deliberately allocated on the handler stack (SRAM1) and never in
  *        SRAM2 - the memory that just reported a parity error must not be
  *        trusted to carry its own failure report.
  */
typedef struct {
    uint32_t magic;         /* SRAM2_PARITY_RECORD_MAGIC                     */
    uint32_t event_id;      /* SRAM2_EVENT_*                                 */
    uint32_t cfgr2;         /* SYSCFG->CFGR2 (SPF, SPL, ECCL, PVDL, CLL)     */
    uint32_t scsr;          /* SYSCFG->SCSR  (SRAM2BSY, SRAM2ER)             */
    uint32_t optr;          /* FLASH->OPTR   (SRAM2_PE, SRAM2_RST)           */
    uint32_t rcc_cifr;      /* RCC->CIFR     (CSSF for a clock-security NMI) */
    uint32_t icsr;          /* SCB->ICSR     (active/pending exceptions)      */
    uint32_t region_start;  /* first byte of the protected SRAM2 window      */
    uint32_t region_end;    /* one past the last byte of that window         */
    uint32_t error_count;   /* parity events seen since power-on             */
} sram2_parity_record_t;

/**
  * @brief  Initialise SRAM2 for parity-protected use.
  *
  * Erases SRAM2 in hardware (which also writes valid parity for every byte),
  * copies the initialised .sram2 image from Flash, clears any stale parity
  * flag and reports whether the SRAM2_PE option byte actually enables the
  * hardware check.
  *
  * @note MUST be called before any SRAM2_CRITICAL / SRAM2_CRITICAL_NOINIT
  *       object is read or written - the hardware erase wipes the whole block.
  */
void sram2_parity_init(void);

/** @brief Parity check status as read back from the FLASH option bytes. */
sram2_parity_status_t sram2_parity_get_status(void);

/** @brief Non-zero when the SRAM2 parity check is enabled in the option byte. */
int sram2_parity_is_enabled(void);

/** @brief Number of parity events observed since power-on (counter in SRAM1). */
uint32_t sram2_parity_error_count(void);

/**
  * @brief  Restore an SRAM2_CRITICAL object from its Flash load image.
  * @param  obj  address of an object placed in the .sram2 section.
  * @param  len  size of that object in bytes.
  * @retval 0 on success, -1 if [obj, obj+len) is not inside .sram2.
  * @note   The load image is the immutable copy the linker kept in Flash, so
  *         this re-establishes the compile-time defaults (and valid parity)
  *         for a structure found corrupted at runtime. Also the building
  *         block for the periodic scrubbing planned in W2-5.
  */
int sram2_restore_from_image(void *obj, size_t len);

/**
 * @brief  True when @p obj lies inside the initialised .sram2 section.
 * @retval 1 if obj is an object that has a Flash load image (so it can be
 *         restored from that image / contained by a reboot), 0 otherwise.
 * @note   Excludes .sram2_noinit objects, which carry no load image.
 */
int sram2_section_contains(const void *obj);

/**
  * @brief NMI back end: record the fault in LastStates and reset the OBSW.
  * @note  Called from NMI_Handler() in stm32l4xx_it.c. Does not return.
  */
void sram2_parity_nmi_handler(void);

#if defined(SRAM2_PARITY_PROGRAM_OPTION_BYTE) && (SRAM2_PARITY_PROGRAM_OPTION_BYTE == 1)
/**
  * @brief  Program the SRAM2_PE / SRAM2_RST user option bytes (one shot).
  * @retval 0 on success (the MCU resets and never returns), -1 on failure.
  * @note   Opt-in at build time: option bytes are non-volatile and their
  *         launch triggers a system reset, so flight builds normally have the
  *         bits set by the flashing procedure instead.
  */
int sram2_parity_program_option_byte(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SRAM2_PARITY_H */
