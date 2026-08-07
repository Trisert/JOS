/* tools/crc_selftest.c — host-side gate for the boot integrity check.
 *
 * The image is stamped by tools/fw_crc_stamp.py (zlib.crc32) but verified
 * on orbit by boot_crc32() (App/obsw/boot_crc.c, nibble-table CRC-32). If the
 * two ever diverge, every board would boot into the safe state — or, worse,
 * a corrupted image would pass. This test compiles the *flight* routine for
 * the host and runs it over the stamped artefact, so the divergence fails the
 * build instead of the mission.
 *
 * Build/run via `make crc-selftest` (see Makefile).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_CRC_HOST_BUILD 1
#include "boot_crc.h"

#define CRC_WORD_SIZE 4u

/* Known-answer test: CRC-32/ISO-HDLC of "123456789" (zlib check value). */
static int known_answer_test(void)
{
    static const char vector[] = "123456789";
    const uint32_t expected = 0xCBF43926u;
    uint32_t got = boot_crc32(vector, sizeof(vector) - 1u);

    if (got != expected) {
        fprintf(stderr,
                "crc_selftest: known-answer test FAILED: boot_crc32(\"123456789\")"
                " = 0x%08X, expected 0x%08X\n", got, expected);
        return 1;
    }
    printf("crc_selftest: known-answer test OK (0x%08X)\n", got);
    return 0;
}

static int image_test(const char *path)
{
    FILE *fh = fopen(path, "rb");
    long size;
    uint8_t *image;
    uint32_t stored;
    uint32_t computed;

    if (fh == NULL) {
        fprintf(stderr, "crc_selftest: cannot open %s\n", path);
        return 1;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        fclose(fh);
        fprintf(stderr, "crc_selftest: seek failed on %s\n", path);
        return 1;
    }
    size = ftell(fh);
    rewind(fh);
    if (size <= (long)CRC_WORD_SIZE) {
        fclose(fh);
        fprintf(stderr, "crc_selftest: %s is too small (%ld bytes)\n", path, size);
        return 1;
    }

    image = (uint8_t *)malloc((size_t)size);
    if (image == NULL) {
        fclose(fh);
        fprintf(stderr, "crc_selftest: out of memory\n");
        return 1;
    }
    if (fread(image, 1u, (size_t)size, fh) != (size_t)size) {
        fclose(fh);
        free(image);
        fprintf(stderr, "crc_selftest: short read on %s\n", path);
        return 1;
    }
    fclose(fh);

    /* Same region the firmware checks: everything up to the stored word.
       The .bin is little-endian, as is the target. */
    computed = boot_crc32(image, (size_t)size - CRC_WORD_SIZE);
    stored = (uint32_t)image[size - 4] |
             ((uint32_t)image[size - 3] << 8) |
             ((uint32_t)image[size - 2] << 16) |
             ((uint32_t)image[size - 1] << 24);
    free(image);

    printf("crc_selftest: %s (%ld bytes)\n", path, size);
    printf("  stored   : 0x%08X\n", stored);
    printf("  computed : 0x%08X (flight boot_crc32)\n", computed);

    if (stored == BOOT_CRC_UNSTAMPED_VALUE) {
        fprintf(stderr, "crc_selftest: image is UNSTAMPED — run `make crc-stamp`\n");
        return 1;
    }
    if (stored == BOOT_CRC_ERASED_VALUE) {
        fprintf(stderr, "crc_selftest: CRC word is the erased-Flash pattern\n");
        return 1;
    }
    if (stored != computed) {
        fprintf(stderr, "crc_selftest: MISMATCH — the flight routine disagrees "
                        "with the stamped value; the OBC would boot into the "
                        "safe state\n");
        return 1;
    }

    printf("  result   : OK — on-orbit check will pass on this image\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <stamped-firmware.bin>\n", argv[0]);
        return 2;
    }
    if (known_answer_test() != 0) {
        return 1;
    }
    return image_test(argv[1]);
}
