#ifndef DUAL_BANK_H
#define DUAL_BANK_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------------
 * W2-2 — Dual-bank golden-image fallback
 *
 * Rationale: NASA-STD-8739.8 (graceful degradation / fault tolerance) and
 * ECSS-Q-ST-80C §6.2.6 (software fault tolerance and recovery) require that a
 * single corrupted artefact must not leave the spacecraft without a bootable
 * image. RedPill therefore keeps a known-good "golden" image in Flash bank 2
 * and falls back to it when the primary image is unusable.
 *
 * Trigger conditions (evaluated once per boot, in dual_bank_init()):
 *   1. the primary image fails its boot CRC32 (boot_crc_verify() ==
 *      BOOT_CRC_MISMATCH — see App/obsw/boot_crc.h), or
 *   2. the persisted boot-fault counter reaches
 *      DUAL_BANK_BOOT_FAULT_THRESHOLD, i.e. the primary image has taken a
 *      HardFault/NMI during early boot that many times in a row.
 *
 * Recovery action: arm BFB2 (boot-from-bank-2 option bit) and reset. The
 * STM32L496 boot logic then maps bank 2 at 0x08000000 and executes the golden
 * image; the (corrupt) primary image stays readable at 0x08080000 for
 * forensics and for re-upload from ground.
 *
 * ---------------------------------------------------------------------------
 * SAFETY / ANTI-BRICK CONTRACT  (read before changing anything here)
 *
 * Arming BFB2 with an unbootable bank 2 bricks the spacecraft: the boot ROM
 * would load a garbage MSP/PC and no ground command could recover it without
 * physical access to BOOT0. dual_bank_switch_to_golden() therefore refuses to
 * touch the option bytes unless ALL of the following hold:
 *
 *   G1  the golden slot is not overlapped by another Flash user
 *       (compile-time, DUAL_BANK_GOLDEN_SLOT_AVAILABLE below);
 *   G2  the device really is in dual-bank mode (DUALBANK option bit) with two
 *       512 KB banks;
 *   G3  the golden image carries a valid trailer (magic + length + CRC32 +
 *       inverted CRC32) and its CRC32 recomputes correctly;
 *   G4  the golden vector table is sane: MSP inside SRAM, reset vector inside
 *       the *linked* Flash window (0x08000000..0x0807FFFF — the golden image
 *       is linked for the bank-1 view because BFB2 remaps bank 2 there) with
 *       the Thumb bit set;
 *   G5  we are not already running from the golden image (no ping-pong).
 *
 * If any gate fails the OBSW keeps running the primary image and reports the
 * degraded state through dual_bank_get_status() — degrade, never brick.
 *
 * ---------------------------------------------------------------------------
 * CURRENT LAYOUT LIMITATION (why the fallback is inhibited on this build)
 *
 * The LastStates forensic pool lives at 0x08080000 (8 KB) — which is exactly
 * the base of Flash bank 2, i.e. the address the boot ROM fetches the golden
 * vector table from after a BFB2 swap. A golden image and the LastStates pool
 * cannot both own that address. Until the pool is relocated (proposal: top of
 * bank 2, 0x080FE000, which requires App/memory/memory.c, the LASTSTATES
 * linker region and the ground forensics tooling to move together), gate G1
 * evaluates false at compile time and the switch is permanently inhibited:
 * the mechanism is built, exercised and self-testing, but it will never
 * program BFB2 into a configuration that cannot boot.
 *
 * Once the pool is relocated, rebuild with (for example)
 *     -DDUAL_BANK_LASTSTATES_BASE=0x080FE000U
 * and the fallback arms itself with no code change.
 * ------------------------------------------------------------------------- */

/* ---------- Flash geometry (STM32L496VGTx, 1 MB, always dual bank) -------- */
#ifndef DUAL_BANK_FLASH_BASE
#define DUAL_BANK_FLASH_BASE        0x08000000U
#endif
#ifndef DUAL_BANK_BANK_SIZE
#define DUAL_BANK_BANK_SIZE         (512U * 1024U)
#endif
#define DUAL_BANK_BANK2_BASE        (DUAL_BANK_FLASH_BASE + DUAL_BANK_BANK_SIZE)

/* Golden image slot: bank 2 base. This is not a free choice — the boot ROM
 * fetches the vector table from the base of the bank selected by BFB2. */
#ifndef DUAL_BANK_GOLDEN_BASE
#define DUAL_BANK_GOLDEN_BASE       DUAL_BANK_BANK2_BASE
#endif

/* LastStates forensic pool (App/memory/memory.c LASTSTATES_FLASH_BASE and the
 * LASTSTATES region in STM32L496VGTX_FLASH.ld must agree with this). */
#ifndef DUAL_BANK_LASTSTATES_BASE
#define DUAL_BANK_LASTSTATES_BASE   0x08080000U
#endif
#define DUAL_BANK_LASTSTATES_SIZE   (8U * 1024U)

/* Smallest credible golden image (vector table + a little code). Used for the
 * compile-time overlap test and to reject nonsense trailer lengths. */
#define DUAL_BANK_GOLDEN_MIN_SIZE   0x400U

/* G1: golden slot must not overlap the LastStates pool. */
#if ((DUAL_BANK_GOLDEN_BASE) < ((DUAL_BANK_LASTSTATES_BASE) + (DUAL_BANK_LASTSTATES_SIZE))) && \
    (((DUAL_BANK_GOLDEN_BASE) + (DUAL_BANK_GOLDEN_MIN_SIZE)) > (DUAL_BANK_LASTSTATES_BASE))
#define DUAL_BANK_GOLDEN_SLOT_AVAILABLE 0
#else
#define DUAL_BANK_GOLDEN_SLOT_AVAILABLE 1
#endif

/* ---------- Golden image trailer ----------
 * Written by ground tooling at the very top of bank 2 when the golden image
 * is uploaded. Keeping the descriptor at a fixed address (rather than inside
 * the image) lets the primary image validate a golden image of any size
 * without knowing how it was linked. */
#define DUAL_BANK_GOLDEN_MAGIC      0x4E444C47U   /* 'G','L','D','N' (LE) */

typedef struct {
    uint32_t magic;      /* DUAL_BANK_GOLDEN_MAGIC                        */
    uint32_t length;     /* bytes covered by crc32, from the golden base  */
    uint32_t crc32;      /* boot_crc32() over [base, base+length)         */
    uint32_t crc32_inv;  /* ~crc32 — guards the descriptor itself         */
} dual_bank_golden_trailer_t;

#define DUAL_BANK_TRAILER_ADDR \
    ((DUAL_BANK_BANK2_BASE) + (DUAL_BANK_BANK_SIZE) - sizeof(dual_bank_golden_trailer_t))

/* ---------- Boot-fault counter ----------
 * Incremented by dual_bank_mark_boot_fault() from the HardFault/NMI handlers
 * while the kernel is not running yet, persisted to the LastStates pool on
 * the following boot, and cleared by dual_bank_boot_complete() once a boot
 * has reached the scheduler. Three consecutive failed boots is the standard
 * "the image cannot come up" evidence threshold. */
#ifndef DUAL_BANK_BOOT_FAULT_THRESHOLD
#define DUAL_BANK_BOOT_FAULT_THRESHOLD  3U
#endif

/* ---------- Status ---------- */
typedef enum {
    DUAL_BANK_PRIMARY_OK      = 0,  /* running primary, nothing to do        */
    DUAL_BANK_ON_GOLDEN       = 1,  /* running the golden image (BFB2 set)   */
    DUAL_BANK_FALLBACK_ARMED  = 2,  /* switch requested; reset is imminent   */
    DUAL_BANK_DEGRADED        = 3,  /* fallback needed but no valid golden   */
    DUAL_BANK_INHIBITED       = 4,  /* fallback disabled by layout (G1)      */
    DUAL_BANK_NOT_DUAL        = 5,  /* option bytes report single-bank Flash */
    DUAL_BANK_ERROR           = 6,  /* option-byte access failed             */
} dual_bank_status_t;

/* Return codes of dual_bank_switch_to_golden(). A successful call does not
 * return (the device resets). */
typedef enum {
    DUAL_BANK_SWITCH_INHIBITED   = -1,  /* G1 failed                        */
    DUAL_BANK_SWITCH_NO_GOLDEN   = -2,  /* G3/G4 failed                     */
    DUAL_BANK_SWITCH_NOT_DUAL    = -3,  /* G2 failed                        */
    DUAL_BANK_SWITCH_ALREADY     = -4,  /* G5: already on golden            */
    DUAL_BANK_SWITCH_OB_FAILED   = -5,  /* option-byte programming failed   */
} dual_bank_switch_err_t;

/* ---------- API ---------- */

/* Read the option bytes, work out which bank is executing, fold the boot-fault
 * evidence, and fall back to the golden image if the primary one is unusable.
 * Call from main() after boot_crc_verify() and before osKernelStart().
 * Returns the latched status (see dual_bank_get_status()). */
dual_bank_status_t dual_bank_init(void);

/* Record one boot-phase fault. ISR-safe and re-entrant: touches only a
 * magic-guarded scratch word in SRAM1 (.boot_fault, NOLOAD), never Flash —
 * programming Flash from a fault handler can hang forever because HAL's
 * timeout relies on SysTick, which cannot preempt HardFault. The value is
 * written through to the LastStates pool by the next dual_bank_init(). */
void dual_bank_mark_boot_fault(void);

/* Declare this boot successful: clears the RAM scratch and, if faults had
 * been persisted, appends a boot-OK marker to LastStates so the counter
 * restarts from zero. Call once, just before osKernelStart(). */
void dual_bank_boot_complete(void);

/* Arm BFB2 and reset into the golden image. Runs gates G1..G5 first and
 * returns a negative dual_bank_switch_err_t if any of them fails; on success
 * it does not return. */
int dual_bank_switch_to_golden(void);

/* ---------- Accessors (telemetry / beacon) ---------- */
dual_bank_status_t dual_bank_get_status(void);
uint32_t dual_bank_active_bank(void);        /* 1 = primary, 2 = golden       */
uint32_t dual_bank_boot_fault_count(void);   /* faults since last good boot   */
bool     dual_bank_golden_valid(void);       /* result of the last G3/G4 run  */
uint32_t dual_bank_get_optr_snapshot(void);  /* user option bytes as read     */

/* Re-run the golden-image validation (G3/G4). Read-only; exposed for ground
 * commanded self-tests and for unit testing. */
bool dual_bank_verify_golden(void);

#endif /* DUAL_BANK_H */
