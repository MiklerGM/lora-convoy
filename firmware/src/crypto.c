#include "crypto.h"

#include <string.h>

/* ---- AES-128, FIPS-197 ------------------------------------------------------------ */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

void aes128_init(aes128_t *ctx, const uint8_t key[16]) {
    uint8_t *w = ctx->round_key;
    memcpy(w, key, 16);
    uint8_t rcon = 0x01;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
        if (i % 16 == 0) {
            /* RotWord, SubWord, Rcon */
            const uint8_t first = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; j++) w[i + j] = (uint8_t)(w[i - 16 + j] ^ t[j]);
    }
}

void aes128_encrypt(const aes128_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ ctx->round_key[i]);
    for (int round = 1; round <= 10; round++) {
        uint8_t t[16];
        /* SubBytes and ShiftRows together: byte (row r, column c) comes from column c + r. The state
         * is column after column, so row r of column c is s[4c + r]. */
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) t[4 * c + r] = SBOX[s[4 * ((c + r) % 4) + r]];
        }
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                const uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                /* MixColumns as 2a0 ^ 3a1 ^ a2 ^ a3 = a0 ^ all ^ xtime(a0 ^ a1), and so on */
                t[4 * c] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                t[4 * c + 1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                t[4 * c + 2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                t[4 * c + 3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ ctx->round_key[16 * round + i]);
    }
    memcpy(out, s, 16);
}

/* ---- CCM, NIST SP 800-38C / RFC 3610, L = 2 --------------------------------------- */

#define CCM_L 2

/* CBC-MAC over B0, the associated data and the message (RFC 3610, 2.2), into mac. */
static void cbc_mac(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
                    const uint8_t *msg, size_t len, size_t tag_len, uint8_t mac[16]) {
    uint8_t b[16];
    b[0] = (uint8_t)((aad_len ? 0x40 : 0) | ((tag_len - 2) / 2) << 3 | (CCM_L - 1));
    memcpy(&b[1], nonce, CCM_NONCE_LEN);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    aes128_encrypt(key, b, mac);
    if (aad_len) {
        /* l(a) in two bytes (the associated data here is short), then a, zero-padded to a block */
        size_t at = 2, i = 0;
        memset(b, 0, 16);
        b[0] = (uint8_t)(aad_len >> 8);
        b[1] = (uint8_t)aad_len;
        while (i < aad_len) {
            b[at++] = aad[i++];
            if (at == 16 || i == aad_len) {
                for (int j = 0; j < 16; j++) mac[j] ^= b[j];
                aes128_encrypt(key, mac, mac);
                memset(b, 0, 16);
                at = 0;
            }
        }
    }
    for (size_t i = 0; i < len; i += 16) {
        const size_t n = len - i < 16 ? len - i : 16;
        for (size_t j = 0; j < n; j++) mac[j] ^= msg[i + j];
        aes128_encrypt(key, mac, mac);
    }
}

/* Block i of the key stream: A_i = flags (L - 1), the nonce, the counter i (RFC 3610, 2.3). */
static void stream_block(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], uint16_t i, uint8_t out[16]) {
    uint8_t a[16];
    a[0] = CCM_L - 1;
    memcpy(&a[1], nonce, CCM_NONCE_LEN);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    aes128_encrypt(key, a, out);
}

static void ctr(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *in, size_t len, uint8_t *out) {
    for (size_t i = 0; i < len; i += 16) {
        uint8_t s[16];
        stream_block(key, nonce, (uint16_t)(i / 16 + 1), s);
        const size_t n = len - i < 16 ? len - i : 16;
        for (size_t j = 0; j < n; j++) out[i + j] = (uint8_t)(in[i + j] ^ s[j]);
    }
}

void ccm_seal(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
              const uint8_t *pt, size_t len, uint8_t *ct, uint8_t *tag, size_t tag_len) {
    uint8_t mac[16], s0[16];
    cbc_mac(key, nonce, aad, aad_len, pt, len, tag_len, mac);
    stream_block(key, nonce, 0, s0);
    for (size_t j = 0; j < tag_len; j++) tag[j] = (uint8_t)(mac[j] ^ s0[j]);
    ctr(key, nonce, pt, len, ct);
}

bool ccm_open(const aes128_t *key, const uint8_t nonce[CCM_NONCE_LEN], const uint8_t *aad, size_t aad_len,
              const uint8_t *ct, size_t len, const uint8_t *tag, size_t tag_len, uint8_t *pt) {
    uint8_t mac[16], s0[16];
    ctr(key, nonce, ct, len, pt);
    cbc_mac(key, nonce, aad, aad_len, pt, len, tag_len, mac);
    stream_block(key, nonce, 0, s0);
    uint8_t diff = 0; /* every byte compared, so a reply's timing says nothing about where it differed */
    for (size_t j = 0; j < tag_len; j++) diff |= (uint8_t)(tag[j] ^ mac[j] ^ s0[j]);
    if (diff) memset(pt, 0, len);
    return diff == 0;
}
