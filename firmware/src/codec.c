#include "codec.h"

#include <string.h>

/*
 *  in clear, authenticated:
 *  0     version (2 bits) | type (2 bits) | flags (4 bits, zero)
 *  1     group tag
 *  2     slot
 *  3-6   session, big-endian
 *  7-8   counter, big-endian
 *  encrypted:
 *  9-10  fix time, big-endian
 *  11-13 latitude, signed, 90 degrees = 2^23
 *  14-16 longitude, signed, 180 degrees = 2^23
 *  17    speed, km/h
 *  18    course, 2-degree steps
 *  19    status in bits 3-0; the call for help's number in bits 6-4, 0 without one
 *  20    help heard: a call for help the sender hears, the caller's slot in bits 4-0 and the
 *        call's number in bits 7-5; 0 for none
 *  21-24 tag
 *
 * Nonce, 13 bytes: slot, session, counter, then zeros. A sender's slot and session make it unique
 * even when two radios are misconfigured onto one slot; the counter within the session.
 */

#define SCALE (1 << 23)

void group_key_init(group_key_t *g, const uint8_t key[16]) {
    aes128_t k;
    aes128_init(&k, key);
    uint8_t block[16] = {0xFE, 'c', 'o', 'n', 'v', 'o', 'y', '-', 'c', 'c', 'm'}, out[16];
    aes128_encrypt(&k, block, out);
    aes128_init(&g->ccm, out);
    const uint8_t tag_block[16] = {0xFF, 'c', 'o', 'n', 'v', 'o', 'y', '-', 't', 'a', 'g'};
    aes128_encrypt(&k, tag_block, out);
    g->tag = out[0];
    g->id = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 | out[3];
    const uint8_t frame_block[16] = {0xFD, 'c', 'o', 'n', 'v', 'o', 'y', '-', 'f', 'r', 'a', 'm', 'e'};
    aes128_encrypt(&k, frame_block, out);
    aes128_init(&g->frame, out);
    memset(&k, 0, sizeof(k));
    memset(out, 0, sizeof(out));
}

/* Rounds half away from zero, and keeps +90 / +180 inside 24 bits. */
static int32_t to24(int32_t e7, int64_t full_e7) {
    int64_t num = (int64_t)e7 * SCALE;
    int64_t v = num >= 0 ? (num + full_e7 / 2) / full_e7 : -((-num + full_e7 / 2) / full_e7);
    if (v > SCALE - 1) v = SCALE - 1;
    if (v < -SCALE) v = -SCALE;
    return (int32_t)v;
}

static int32_t from24(int32_t v, int64_t full_e7) {
    int64_t num = (int64_t)v * full_e7;
    return (int32_t)(num >= 0 ? (num + SCALE / 2) / SCALE : -((-num + SCALE / 2) / SCALE));
}

static void put24(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 16);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)v;
}

static uint32_t get24(const uint8_t *b) { return (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]; }

static int32_t sign24(uint32_t v) { return (v & 0x800000u) ? (int32_t)v - 0x1000000 : (int32_t)v; }

static void nonce_of(const uint8_t *header, uint8_t nonce[CCM_NONCE_LEN]) {
    memset(nonce, 0, CCM_NONCE_LEN);
    memcpy(nonce, &header[2], 7); /* slot, session, counter */
}

size_t pkt_seal(uint8_t out[PKT_POSITION_LEN], const position_t *p, const group_key_t *g) {
    out[0] = (uint8_t)(PKT_VERSION << 6 | PKT_TYPE_POSITION << 4);
    out[1] = g->tag;
    out[2] = p->slot;
    out[3] = (uint8_t)(p->session >> 24);
    out[4] = (uint8_t)(p->session >> 16);
    out[5] = (uint8_t)(p->session >> 8);
    out[6] = (uint8_t)p->session;
    out[7] = (uint8_t)(p->counter >> 8);
    out[8] = (uint8_t)p->counter;
    uint8_t body[PKT_BODY_LEN];
    body[0] = (uint8_t)(p->fix_time >> 8);
    body[1] = (uint8_t)p->fix_time;
    put24(&body[2], (uint32_t)to24(p->lat_e7, 900000000ll) & 0xFFFFFFu);
    put24(&body[5], (uint32_t)to24(p->lon_e7, 1800000000ll) & 0xFFFFFFu);
    body[8] = p->speed_kmh;
    body[9] = p->course2;
    body[10] = (uint8_t)((p->status & 0x0F) | (p->call & 7) << 4);
    body[11] = p->help_heard == HELP_HEARD_NONE ? 0 : (uint8_t)((p->heard_call & 7) << 5 | (p->help_heard & 0x1F));
    uint8_t nonce[CCM_NONCE_LEN];
    nonce_of(out, nonce);
    ccm_seal(&g->ccm, nonce, out, PKT_HEADER_LEN, body, PKT_BODY_LEN, &out[PKT_HEADER_LEN],
             &out[PKT_HEADER_LEN + PKT_BODY_LEN], PKT_TAG_LEN);
    return PKT_POSITION_LEN;
}

pkt_result_t pkt_open(const uint8_t *buf, size_t len, const group_key_t *g, position_t *p) {
    if (len != PKT_POSITION_LEN || buf[0] != (PKT_VERSION << 6 | PKT_TYPE_POSITION << 4)) return PKT_MALFORMED;
    if (buf[1] != g->tag) return PKT_FOREIGN;
    uint8_t nonce[CCM_NONCE_LEN], body[PKT_BODY_LEN];
    nonce_of(buf, nonce);
    if (!ccm_open(&g->ccm, nonce, buf, PKT_HEADER_LEN, &buf[PKT_HEADER_LEN], PKT_BODY_LEN,
                  &buf[PKT_HEADER_LEN + PKT_BODY_LEN], PKT_TAG_LEN, body)) {
        return PKT_FORGED;
    }
    p->group = buf[1];
    p->slot = buf[2];
    p->session = (uint32_t)buf[3] << 24 | (uint32_t)buf[4] << 16 | (uint32_t)buf[5] << 8 | buf[6];
    p->counter = (uint16_t)(buf[7] << 8 | buf[8]);
    p->fix_time = (uint16_t)(body[0] << 8 | body[1]);
    p->lat_e7 = from24(sign24(get24(&body[2])), 900000000ll);
    p->lon_e7 = from24(sign24(get24(&body[5])), 1800000000ll);
    p->speed_kmh = body[8];
    p->course2 = body[9];
    p->status = body[10] & 0x0F;
    p->call = body[10] >> 4 & 7;
    p->heard_call = body[11] >> 5;
    p->help_heard = p->heard_call ? body[11] & 0x1F : HELP_HEARD_NONE;
    return PKT_OK;
}
