#include "boot_crc.h"

#if !defined(BOOT_CRC_HOST_BUILD)
#include "main.h"        /* NVIC_SystemReset(), HAL_GetTick(), __DSB()      */
#include "memory.h"      /* laststates_write()                              */
#include "obsw_types.h"  /* laststates_entry_t, TRIGGER_IMAGE_CRC_FAIL      */
#include <string.h>
#endif

/* ---------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3), reflected, poly 0xEDB88320, init 0xFFFFFFFF,
 * final XOR 0xFFFFFFFF — bit-identical to zlib.crc32().
 *
 * Nibble-wise table (16 entries, 64 B of .rodata) instead of the usual 1 KiB
 * byte table: ~4x faster than the bitwise loop while keeping the Flash cost
 * negligible on a 512 KB image budget.
 *
 * `make crc-selftest` compiles this routine for the host and runs it over the
 * stamped .bin, so any divergence from the zlib-based stamping tool fails the
 * build instead of bricking a board.
 * ------------------------------------------------------------------------- */
static const uint32_t crc32_nibble_table[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t boot_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (p == NULL) {
        return 0u;
    }

    while (len-- > 0u) {
        crc ^= (uint32_t)(*p++);
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
    }

    return crc ^ 0xFFFFFFFFu;
}

#if !defined(BOOT_CRC_HOST_BUILD)

/* ---------------------------------------------------------------------------
 * Linker-provided image bounds (STM32L496VGTX_FLASH.ld):
 *   __fw_image_start = ORIGIN(FLASH)         (0x08000000)
 *   __fw_crc_start   = start of .fw_crc      (last loaded section)
 * Declared as arrays so the symbol *address* is the value we need.
 * ------------------------------------------------------------------------- */
extern const uint8_t __fw_image_start[];
extern const uint8_t __fw_crc_start[];

/* ---------------------------------------------------------------------------
 * Stored (expected) CRC.
 *
 * Emitted into the .fw_crc section, i.e. the last 4 bytes of the binary
 * image, and left at BOOT_CRC_UNSTAMPED_VALUE (0x00000000) until
 * `make crc-stamp` (tools/fw_crc_stamp.py) patches the built .bin/.hex.
 * `used` + KEEP() in the linker script prevent -ffunction-sections /
 * --gc-sections from dropping it.
 *
 * The placeholder is 0x00000000 and NOT the erased-Flash pattern
 * 0xFFFFFFFF: an erased/decayed CRC word must be a fault, not a free pass.
 * ------------------------------------------------------------------------- */
__attribute__((section(".fw_crc"), used))
const volatile uint32_t fw_crc_stored = BOOT_CRC_UNSTAMPED_VALUE;

/* ---------- Latched result (readable by telemetry / beacon) ---------- */
static boot_crc_status_t crc_status     = BOOT_CRC_UNSTAMPED;
static uint32_t          crc_computed   = 0u;
static uint32_t          crc_expected   = BOOT_CRC_UNSTAMPED_VALUE;
static uint32_t          crc_region_len = 0u;

/* ---------------------------------------------------------------------------
 * Warm-reset-persistent retry state.
 *
 * Placed in .noinit (see STM32L496VGTX_FLASH.ld): the startup code only
 * zeroes .bss and copies .data, so these survive NVIC_SystemReset() but are
 * garbage after a cold start — hence the magic word.
 * ------------------------------------------------------------------------- */
#define BOOT_CRC_NOINIT_MAGIC 0x43524346u  /* "CRCF" */

__attribute__((section(".noinit"))) static volatile uint32_t crc_boot_magic;
__attribute__((section(".noinit"))) static volatile uint32_t crc_boot_attempts;

boot_crc_status_t boot_crc_verify(void)
{
    const uint8_t *start = __fw_image_start;
    const uint8_t *end   = __fw_crc_start;

    if (end <= start) {
        /* Linker layout broken: cannot verify anything. Treated as a fault,
           not as "nothing to check". */
        crc_status     = BOOT_CRC_BAD_REGION;
        crc_region_len = 0u;
        return crc_status;
    }

    crc_region_len = (uint32_t)(end - start);
    crc_computed   = boot_crc32(start, (size_t)crc_region_len);
    crc_expected   = fw_crc_stored;

    if (crc_expected == BOOT_CRC_UNSTAMPED_VALUE) {
        /* Deliberate placeholder: image was never stamped (bench build
           flashed straight from the .elf). Nothing to compare against —
           report, do not fail. CI never ships such an artefact: `make all`
           depends on crc-stamp and the workflow re-runs `make crc-check`. */
        crc_status = BOOT_CRC_UNSTAMPED;
    } else if (crc_expected == BOOT_CRC_ERASED_VALUE) {
        /* Erased Flash where a stamp should be: the image tail was never
           programmed or has been corrupted. Never a valid stamp. */
        crc_status = BOOT_CRC_ERASED;
    } else if (crc_computed == crc_expected) {
        crc_status = BOOT_CRC_OK;
    } else {
        crc_status = BOOT_CRC_MISMATCH;
    }

    return crc_status;
}

int boot_crc_image_trusted(void)
{
    return ((crc_status == BOOT_CRC_OK) || (crc_status == BOOT_CRC_UNSTAMPED))
           ? 1 : 0;
}

/* Persist the integrity fault so it survives the reset and is downlinkable
   through the existing LastStates dump (post-mortem evidence, ECSS-Q-ST-80C
   §6.3.5). Failure to persist must not stop the recovery action. */
static void boot_crc_record_fault(uint32_t attempt)
{
    laststates_entry_t entry;
    uint32_t ctx[5];

    memset(&entry, 0, sizeof(entry));
    entry.timestamp  = HAL_GetTick();      /* pre-scheduler: HAL tick only */
    entry.state_from = (uint8_t)STATE_OFF;
    entry.state_to   = (uint8_t)STATE_CRIT;
    entry.trigger    = (uint8_t)TRIGGER_IMAGE_CRC_FAIL;

    ctx[0] = (uint32_t)crc_status;
    ctx[1] = crc_expected;
    ctx[2] = crc_computed;
    ctx[3] = crc_region_len;
    ctx[4] = attempt;
    memcpy(entry.context, ctx, sizeof(ctx));

    (void)laststates_write(&entry);
}

void boot_crc_apply_policy(void)
{
    if (boot_crc_image_trusted()) {
        /* Healthy boot: arm the counter for a future in-flight corruption. */
        crc_boot_magic    = BOOT_CRC_NOINIT_MAGIC;
        crc_boot_attempts = 0u;
        return;
    }

    /* MISMATCH / ERASED / BAD_REGION: a real integrity fault. */
    if (crc_boot_magic != BOOT_CRC_NOINIT_MAGIC) {
        /* Cold start (or .noinit RAM never initialised): start the budget. */
        crc_boot_magic    = BOOT_CRC_NOINIT_MAGIC;
        crc_boot_attempts = 0u;
    }

    boot_crc_record_fault(crc_boot_attempts);

#if BOOT_CRC_FATAL
    if (crc_boot_attempts < BOOT_CRC_MAX_RESET_ATTEMPTS) {
        crc_boot_attempts++;
        __DSB();
        NVIC_SystemReset();   /* does not return */
    }
#endif

    /* Retry budget exhausted (or BOOT_CRC_FATAL=0 bench build): continue
       booting, but the image stays untrusted. boot_crc_image_trusted() now
       returns 0, which confines the state machine to STATE_CRIT — beacon
       only, payloads inhibited — so ground can re-upload. Deliberately no
       __disable_irq(); while(1): with no IWDG configured that would be an
       unrecoverable brick. */
}

boot_crc_status_t boot_crc_get_status(void)         { return crc_status; }
uint32_t          boot_crc_get_computed(void)       { return crc_computed; }
uint32_t          boot_crc_get_expected(void)       { return crc_expected; }
uint32_t          boot_crc_get_region_len(void)     { return crc_region_len; }
uint32_t          boot_crc_get_reset_attempts(void) { return crc_boot_attempts; }

#endif /* !BOOT_CRC_HOST_BUILD */
