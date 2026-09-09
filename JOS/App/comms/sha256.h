/*
 * sha256.h — SHA-256 + HMAC-SHA256, ported verbatim (algorithm) from the
 * upstream RedPill STM32/LoRa reference
 * (satellite/stm32_lora/Core/{Inc/sha256.h,Src/sha256.c}, author aless).
 * Only the file header and this provenance note are JOS additions; the
 * transform, padding and HMAC envelope are byte-for-byte identical so ground
 * tools validated against upstream produce identical tags.
 *
 * Used by App/comms/comms_validate.c for authenticated uplink (truncated
 * HMAC-SHA256 telecommand tags). No dynamic allocation, no blocking.
 */

#ifndef JOS_COMMS_SHA256_H
#define JOS_COMMS_SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint8_t data[64];
	uint32_t datalen;
	uint64_t bitlen;
	uint32_t state[8];
} SHA256_CTX;

void sha256_init(SHA256_CTX *ctx);
void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len);
void sha256_final(SHA256_CTX *ctx, uint8_t hash[]);
/* HMAC-SHA256 helper. */
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t data_len, uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif
