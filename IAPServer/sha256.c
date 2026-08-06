/*
 * sha256.c
 *
 * Standalone SHA-256 (FIPS 180-4) + HMAC-SHA256 (RFC 2104), no HAL/peripheral
 * dependency. Deliberately self-contained (no malloc, no other project
 * headers) so it can be reused unchanged by the bootloader, and reviewed as
 * a single small unit. See sha256_selftest() for known-vector verification.
 */

#include "sha256.h"
#include <string.h>

static const uint32_t K[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32U - n));
}

static void sha256_process_block(sha256_ctx_t *ctx, const uint8_t block[SHA256_BLOCK_SIZE])
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t i;

	for (i = 0; i < 16U; i++) {
		w[i] = ((uint32_t)block[i * 4U] << 24)
		     | ((uint32_t)block[i * 4U + 1U] << 16)
		     | ((uint32_t)block[i * 4U + 2U] << 8)
		     | ((uint32_t)block[i * 4U + 3U]);
	}
	for (i = 16U; i < 64U; i++) {
		uint32_t s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		uint32_t s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

	for (i = 0; i < 64U; i++) {
		uint32_t S1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t temp1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temp2 = S0 + maj;

		h = g; g = f; f = e; e = d + temp1;
		d = c; c = b; b = a; a = temp1 + temp2;
	}

	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
	ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
	ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
	ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
	ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
	ctx->bitlen = 0;
	ctx->buf_len = 0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
	ctx->bitlen += (uint64_t)len * 8U;

	while (len > 0U) {
		uint32_t space = SHA256_BLOCK_SIZE - ctx->buf_len;
		uint32_t take = (len < space) ? len : space;

		memcpy(ctx->buf + ctx->buf_len, data, take);
		ctx->buf_len += take;
		data += take;
		len -= take;

		if (ctx->buf_len == SHA256_BLOCK_SIZE) {
			sha256_process_block(ctx, ctx->buf);
			ctx->buf_len = 0U;
		}
	}
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
	uint64_t bitlen = ctx->bitlen;
	uint8_t pad = 0x80U;
	uint32_t i;

	sha256_update(ctx, &pad, 1U);

	{
		uint8_t zero = 0x00U;
		while (ctx->buf_len != 56U) {
			sha256_update(ctx, &zero, 1U);
		}
	}

	{
		uint8_t lenbuf[8];
		for (i = 0; i < 8U; i++) {
			lenbuf[i] = (uint8_t)(bitlen >> (56U - 8U * i));
		}
		/* Append directly: buf_len is exactly 56, this completes the last block
		 * without re-entering the padding branch above (bitlen already updated
		 * again would be wrong, so bypass sha256_update's bitlen accounting). */
		memcpy(ctx->buf + 56U, lenbuf, 8U);
		sha256_process_block(ctx, ctx->buf);
	}

	for (i = 0; i < 8U; i++) {
		digest[i * 4U]      = (uint8_t)(ctx->state[i] >> 24);
		digest[i * 4U + 1U] = (uint8_t)(ctx->state[i] >> 16);
		digest[i * 4U + 2U] = (uint8_t)(ctx->state[i] >> 8);
		digest[i * 4U + 3U] = (uint8_t)(ctx->state[i]);
	}
}

void sha256(const uint8_t *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
	sha256_ctx_t ctx;
	sha256_init(&ctx);
	sha256_update(&ctx, data, len);
	sha256_final(&ctx, digest);
}

void hmac_sha256(const uint8_t *key, uint32_t key_len,
                  const uint8_t *msg, uint32_t msg_len,
                  uint8_t out[SHA256_DIGEST_SIZE])
{
	uint8_t key_block[SHA256_BLOCK_SIZE];
	uint8_t o_key_pad[SHA256_BLOCK_SIZE];
	uint8_t i_key_pad[SHA256_BLOCK_SIZE];
	uint8_t inner_digest[SHA256_DIGEST_SIZE];
	sha256_ctx_t ctx;
	uint32_t i;

	memset(key_block, 0, sizeof(key_block));
	if (key_len > SHA256_BLOCK_SIZE) {
		sha256(key, key_len, key_block);
	} else {
		memcpy(key_block, key, key_len);
	}

	for (i = 0; i < SHA256_BLOCK_SIZE; i++) {
		o_key_pad[i] = key_block[i] ^ 0x5cU;
		i_key_pad[i] = key_block[i] ^ 0x36U;
	}

	sha256_init(&ctx);
	sha256_update(&ctx, i_key_pad, SHA256_BLOCK_SIZE);
	sha256_update(&ctx, msg, msg_len);
	sha256_final(&ctx, inner_digest);

	sha256_init(&ctx);
	sha256_update(&ctx, o_key_pad, SHA256_BLOCK_SIZE);
	sha256_update(&ctx, inner_digest, SHA256_DIGEST_SIZE);
	sha256_final(&ctx, out);
}

static bool digest_matches_hex(const uint8_t digest[SHA256_DIGEST_SIZE], const char *hex)
{
	uint32_t i;
	for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
		char hi = hex[i * 2U];
		char lo = hex[i * 2U + 1U];
		uint8_t hi_v = (uint8_t)((hi <= '9') ? (hi - '0') : (hi - 'a' + 10));
		uint8_t lo_v = (uint8_t)((lo <= '9') ? (lo - '0') : (lo - 'a' + 10));
		uint8_t expected = (uint8_t)((hi_v << 4) | lo_v);
		if (digest[i] != expected) {
			return false;
		}
	}
	return true;
}

bool sha256_selftest(void)
{
	uint8_t digest[SHA256_DIGEST_SIZE];
	uint8_t hmac_key[20];
	uint32_t i;

	/* FIPS 180-4 vector: SHA-256("") */
	sha256((const uint8_t *)"", 0, digest);
	if (!digest_matches_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) {
		return false;
	}

	/* FIPS 180-4 vector: SHA-256("abc") */
	sha256((const uint8_t *)"abc", 3, digest);
	if (!digest_matches_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
		return false;
	}

	/* RFC 4231 Test Case 1: HMAC-SHA256(key=0x0b*20, "Hi There") */
	for (i = 0; i < sizeof(hmac_key); i++) {
		hmac_key[i] = 0x0bU;
	}
	hmac_sha256(hmac_key, sizeof(hmac_key), (const uint8_t *)"Hi There", 8, digest);
	if (!digest_matches_hex(digest, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7")) {
		return false;
	}

	return true;
}
