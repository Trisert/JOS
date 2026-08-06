/* ---------------------------------------------------------------------------
 * stubs.c - host substitutes for the linker-provided firmware image bounds.
 *
 * App/obsw/boot_crc.c consumes three symbols that only exist once the image
 * has been linked with STM32L496VGTX_FLASH.ld:
 *
 *     extern const uint8_t __fw_image_start[];   // ORIGIN(FLASH)
 *     extern const uint8_t __fw_crc_start[];     // start of .fw_crc
 *     const volatile uint32_t fw_crc_stored;     // inside .fw_crc
 *
 * Correctness notes (these were defects in the first attempt at this file):
 *
 *   - The definitions below must be `const uint8_t[]`, matching the `extern
 *     const uint8_t[]` declarations in boot_crc.c exactly. Defining them as
 *     plain `uint8_t[]` is a type mismatch across translation units
 *     (C11 6.2.7p2) that no compiler diagnoses and that silently changes the
 *     object's section.
 *
 *   - boot_crc_verify() computes `__fw_crc_start - __fw_image_start`.
 *     Subtracting pointers that address two *different* objects is undefined
 *     (C11 6.5.6p9) and yields a garbage region length. So there is exactly
 *     ONE array object here, and __fw_crc_start is defined as a linker-level
 *     alias for its one-past-the-end address, which C11 6.5.6p8 explicitly
 *     permits as the right-hand operand of a subtraction.
 * ------------------------------------------------------------------------- */
#include "host_support.h"
#include "main.h"          /* fakes/main.h - peripheral handle types (W2-6) */
#include "unity.h"

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * The fake image. Deterministic pattern ((i * 7 + 3) ^ (i << 1)) & 0xFF, so
 * its CRC-32 is a fixed known-answer value (HOST_FW_IMAGE_CRC) obtained from
 * an independent implementation (Python zlib.crc32).
 *
 * `const` matches boot_crc.c's `extern const uint8_t __fw_image_start[]`
 * exactly. Nothing ever writes through this symbol (it stands in for a Flash
 * ORIGIN address), so there is no reason to drop the qualifier.
 * ------------------------------------------------------------------------- */
const uint8_t __fw_image_start[HOST_FW_IMAGE_LEN] = {
    0x03u, 0x08u, 0x15u, 0x1Eu, 0x17u, 0x2Cu, 0x21u, 0x3Au,
    0x2Bu, 0x50u, 0x5Du, 0x46u, 0x4Fu, 0x44u, 0x79u, 0x72u,
    0x53u, 0x58u, 0xA5u, 0xAEu, 0xA7u, 0xBCu, 0xB1u, 0x8Au,
    0x9Bu, 0x80u, 0x8Du, 0xF6u, 0xFFu, 0xF4u, 0xE9u, 0xE2u,
    0xA3u, 0xA8u, 0xB5u, 0xBEu, 0xB7u, 0x4Cu, 0x41u, 0x5Au,
    0x4Bu, 0x70u, 0x7Du, 0x66u, 0x6Fu, 0x64u, 0x19u, 0x12u,
    0x33u, 0x38u, 0x05u, 0x0Eu, 0x07u, 0x1Cu, 0x11u, 0xEAu,
    0xFBu, 0xE0u, 0xEDu, 0xD6u, 0xDFu, 0xD4u, 0xC9u, 0xC2u,
};

/* ---------------------------------------------------------------------------
 * __fw_crc_start: emulate the linker script by defining the symbol at a fixed
 * offset inside the object above (GNU as symbol arithmetic, the same
 * mechanism a linker script uses). The literal must be plain decimal, so it
 * is stringified from a separate unsuffixed macro.
 *
 * Normal layout      : __fw_crc_start = &__fw_image_start[64] (one-past-end)
 *                      -> region length 64, verification proceeds.
 * HOST_FW_BAD_REGION : __fw_crc_start = &__fw_image_start[0]
 *                      -> end <= start, the BOOT_CRC_BAD_REGION guard fires.
 *                      Selected only for test_bad_region (see project.yml).
 * ------------------------------------------------------------------------- */
#define HOST_FW_IMAGE_LEN_ASM 64          /* == HOST_FW_IMAGE_LEN, unsuffixed */
#define HOST_STR_(x) #x
#define HOST_STR(x)  HOST_STR_(x)

#ifdef HOST_FW_BAD_REGION
__asm__(".globl __fw_crc_start\n\t"
        ".set   __fw_crc_start, __fw_image_start\n");
#else
__asm__(".globl __fw_crc_start\n\t"
        ".set   __fw_crc_start, __fw_image_start + " HOST_STR(HOST_FW_IMAGE_LEN_ASM) "\n");
#endif

/* ---------------------------------------------------------------------------
 * Stamping the stored CRC word.
 *
 * `fw_crc_stored` is defined by boot_crc.c. The declaration below repeats its
 * type *exactly* (`const volatile uint32_t`) so there is no cross-TU type
 * mismatch. It is a weak reference because support files are linked into every
 * test executable, including the ones that do not link boot_crc.c at all
 * (e.g. test_laststates); an unresolved weak reference is simply NULL instead
 * of a link error.
 *
 * The qualifier must NOT be dropped here: the section attributes of `.fw_crc`
 * come from the *definition* in boot_crc.c, not from this declaration, and a
 * declaration whose type disagrees with the definition is undefined behaviour
 * (C11 6.2.7p2) that buys nothing.
 *
 * Casting the const away in host_fw_crc_slot() is deliberate and is precisely
 * what the real flow does: on target the word sits in Flash and is patched
 * post-link by tools/fw_crc_stamp.py. The round trip through `uintptr_t`
 * launders the qualifiers, so no -Wcast-qual diagnostic is triggered.
 * `volatile` keeps the compiler from caching the value.
 *
 * The page still has to be made writable at run time: a const object is
 * emitted into a read-only-mapped section, so a bare store would segfault.
 * One mprotect() on the containing page is enough and behaves the same on
 * aarch64 (developer machines) and x86_64 (CI).
 * ------------------------------------------------------------------------- */
extern const volatile uint32_t fw_crc_stored __attribute__((weak));

static volatile uint32_t *host_fw_crc_slot(void)
{
    return (volatile uint32_t *)(uintptr_t)&fw_crc_stored;
}

/* Make the page holding the stored-CRC word writable. Idempotent: the mprotect
 * is attempted once per process. */
static int host_fw_crc_make_writable(volatile uint32_t *slot)
{
    static int unprotected = 0;

    long      page_size;
    uintptr_t page_base;

    if (unprotected) {
        return 0;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }

    page_base = (uintptr_t)slot & ~((uintptr_t)page_size - 1u);

    if (mprotect((void *)page_base, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) {
        return -1;
    }

    unprotected = 1;
    return 0;
}

void host_fw_crc_stamp(uint32_t value)
{
    volatile uint32_t *slot = host_fw_crc_slot();

    if (slot == NULL) {
        /* boot_crc.c is not part of this test executable; nothing to stamp. */
        return;
    }

    if (host_fw_crc_make_writable(slot) != 0) {
        TEST_FAIL_MESSAGE("host_fw_crc_stamp: mprotect() could not make the "
                          ".fw_crc page writable");
        return; /* not reached; TEST_FAIL_MESSAGE long-jumps out */
    }

    *slot = value;
}

uint32_t host_fw_crc_stamped_value(void)
{
    const volatile uint32_t *slot = host_fw_crc_slot();

    return (slot != NULL) ? *slot : 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * CubeMX peripheral handles (W2-6)
 *
 * App/ modules reference these as externs that the flight build resolves in
 * Core/Src/main.c (CubeMX generated, not on the host :source: path):
 *
 *     App/comms/comms.c -> extern SPI_HandleTypeDef hspi1;
 *
 * The tests never inspect the contents: every HAL entry point that consumes a
 * handle is a support-file double which ignores it, so these are address-only
 * placeholders.
 *
 * hi2c2 is deliberately NOT here: support/host_flash.c already defines it
 * next to the FRAM emulation that consumes it, and both files are linked into
 * every test executable, so a second definition is a duplicate symbol.
 * ------------------------------------------------------------------------- */
SPI_HandleTypeDef  hspi1;    /* SX1268 LoRa transceiver  */
ADC_HandleTypeDef  hadc1;    /* BMS measurements         */
IWDG_HandleTypeDef hiwdg;    /* independent watchdog     */

/* ---------------------------------------------------------------------------
 * Error_Handler(): the CubeMX trap. On target it disables interrupts and
 * spins (bounded now only by the IWDG). Reaching it from a host test means
 * the code under test gave up, which is never an expected outcome here, so
 * the double fails the run loudly instead of hanging the CI job.
 * ------------------------------------------------------------------------- */
void Error_Handler(void)
{
    TEST_FAIL_MESSAGE("flight code called Error_Handler()");
}
