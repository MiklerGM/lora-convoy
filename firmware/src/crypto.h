#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AES-128 (FIPS-197), encryption only, and CCM on it (NIST SP 800-38C, RFC 3610) with a 13-byte
 * nonce and a two-byte length field. Portable C, so both boards and the host tests run the same
 * code (ARCHITECTURE D13); checked against FIPS-197, RFC 3610 and OpenSSL (test_core.c).
 *
 * Speed does not matter here: one packet of a few dozen bytes every second or two. Table lookups
 * leak timing to whoever can time the chip from close by, which is not this threat model.
 */
typedef struct {
    uint8_t round_key[176];
} aes128_t;

void aes128_init(aes128_t *ctx, const uint8_t key[16]);
void aes128_encrypt(const aes128_t *ctx, const uint8_t in[16], uint8_t out[16]);

#define CCM_NONCE_LEN 13

/* Encrypts pt into ct (same length, may be the same buffer) and writes a tag of tag_len bytes
 * (4, 6, 8, ... 16). aad is authenticated, not encrypted. */
void ccm_seal(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t len, uint8_t *ct, uint8_t *tag, size_t tag_len);

/* Decrypts ct into pt and checks the tag; false, with pt wiped, when it does not match. */
bool ccm_open(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
              const uint8_t *ct, size_t len, const uint8_t *tag, size_t tag_len, uint8_t *pt);
