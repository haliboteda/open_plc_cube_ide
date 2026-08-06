/*
 * sha256.h
 *
 * Small standalone SHA-256 implementation (FIPS 180-4), no HAL/peripheral
 * dependency. Lives next to IAP_server.c because it exists purely to
 * support IAP firmware-integrity verification (image hash + HMAC auth).
 */

#ifndef IAPSERVER_SHA256_H_
#define IAPSERVER_SHA256_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_BLOCK_SIZE  64U
#define SHA256_DIGEST_SIZE 32U

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  buf[SHA256_BLOCK_SIZE];
	uint32_t buf_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/* One-shot convenience wrapper. */
void sha256(const uint8_t *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

/* HMAC-SHA256, RFC 2104. key_len/msg_len in bytes. */
void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *msg, uint32_t msg_len,
                  uint8_t out[SHA256_DIGEST_SIZE]);

/* Runs known FIPS 180-4 / RFC 4231 test vectors against sha256()/hmac_sha256().
 * Returns true if every vector matches. Meant to be called once at bootloader
 * startup (bootloader_state_init) so a hand-written crypto bug fails loudly
 * on real hardware instead of silently producing wrong verification results. */
bool sha256_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* IAPSERVER_SHA256_H_ */
