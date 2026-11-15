#include "codec.h"

#include <string.h>

/*
 *  in clear, authenticated:
 *  0     version (4 bits) | type (4 bits)
 *  1     group tag
 *  2     slot in bits 4-0; bits 7-5 zero
 *  3-4   session, big-endian
 *  5-8   sent: UTC seconds, big-endian
 *  9     count
 *  encrypted:
 *  10    the fix's age when sent (age_code); 255 with no position
 *  11-13 latitude, signed, 90 degrees = 2^23
 *  14-16 longitude, signed, 180 degrees = 2^23
 *  17    speed, km/h
 *  18    course, 2-degree steps
 *  19    facts in bits 3-0, state in bits 7-4
 *  20    heartbeat in bits 2-0, the fix's satellites in bits 6-3; bit 7 zero
 *  then  passengers, 9 bytes each, as many as the length holds:
 *        slot in bits 4-0, bit 5 "has a position"; latitude; longitude; how long ago it said so
 *        itself (age_code); facts and state
 *  last 4 tag
 *
 * Nonce, 13 bytes: slot, session, sent, count, then zeros. A sender never sends twice with one
 * (sent, count); the session keeps two radios misconfigured onto one slot apart.
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
    memcpy(nonce, &header[2], 8); /* slot, session, sent, count */
}

uint8_t age_code(uint32_t s) {
    return s < AGE_MINUTES_FROM ? (uint8_t)s : s < 122 * 60 ? (uint8_t)(118 + s / 60) : AGE_OLDER;
}

uint32_t age_seconds(uint8_t code) {
    return code < AGE_MINUTES_FROM ? code : code < 240 ? (code - 118u) * 60 : 122 * 60;
}

static void put_where(uint8_t *b, const record_t *r) {
    put24(&b[0], (uint32_t)to24(r->has_pos ? r->lat_e7 : 0, 900000000ll) & 0xFFFFFFu);
    put24(&b[3], (uint32_t)to24(r->has_pos ? r->lon_e7 : 0, 1800000000ll) & 0xFFFFFFu);
}

static void get_where(const uint8_t *b, record_t *r) {
    r->lat_e7 = from24(sign24(get24(&b[0])), 900000000ll);
    r->lon_e7 = from24(sign24(get24(&b[3])), 1800000000ll);
}

size_t pkt_seal(uint8_t *out, const packet_t *p, const group_key_t *g) {
    const unsigned n = p->passengers > PKT_PASSENGERS_MAX ? PKT_PASSENGERS_MAX : p->passengers;
    const size_t body_len = PKT_OWN_LEN + n * PKT_PASSENGER_LEN;
    out[0] = (uint8_t)(PKT_VERSION << 4 | PKT_TYPE_STATUS);
    out[1] = g->tag;
    out[2] = p->own.slot & 0x1F;
    out[3] = (uint8_t)(p->session >> 8);
    out[4] = (uint8_t)p->session;
    out[5] = (uint8_t)(p->sent >> 24);
    out[6] = (uint8_t)(p->sent >> 16);
    out[7] = (uint8_t)(p->sent >> 8);
    out[8] = (uint8_t)p->sent;
    out[9] = p->count;
    uint8_t body[PKT_MAX_LEN];
    body[0] = p->own.has_pos ? (p->own.age == AGE_NONE ? AGE_OLDER : p->own.age) : AGE_NONE;
    put_where(&body[1], &p->own);
    body[7] = p->speed_kmh;
    body[8] = p->course2;
    body[9] = (uint8_t)((p->own.facts & 0x0F) | (p->own.state & 0x0F) << 4);
    body[10] = (uint8_t)((p->beat & 7) | (p->sats > 15 ? 15 : p->sats) << 3);
    for (unsigned i = 0; i < n; i++) {
        const record_t *r = &p->passenger[i];
        uint8_t *b = &body[PKT_OWN_LEN + i * PKT_PASSENGER_LEN];
        b[0] = (uint8_t)((r->slot & 0x1F) | (r->has_pos ? 0x20 : 0));
        put_where(&b[1], r);
        b[7] = r->age == AGE_NONE ? AGE_OLDER : r->age;
        b[8] = (uint8_t)((r->facts & 0x0F) | (r->state & 0x0F) << 4);
    }
    uint8_t nonce[CCM_NONCE_LEN];
    nonce_of(out, nonce);
    ccm_seal(&g->ccm, nonce, out, PKT_HEADER_LEN, body, body_len, &out[PKT_HEADER_LEN],
             &out[PKT_HEADER_LEN + body_len], PKT_TAG_LEN);
    return PKT_HEADER_LEN + body_len + PKT_TAG_LEN;
}

pkt_result_t pkt_open(const uint8_t *buf, size_t len, const group_key_t *g, packet_t *p) {
    if (len < PKT_LEN(0) || len > PKT_MAX_LEN || (len - PKT_LEN(0)) % PKT_PASSENGER_LEN ||
        buf[0] != (PKT_VERSION << 4 | PKT_TYPE_STATUS)) {
        return PKT_MALFORMED;
    }
    if (buf[1] != g->tag) return PKT_FOREIGN;
    const size_t body_len = len - PKT_HEADER_LEN - PKT_TAG_LEN;
    uint8_t nonce[CCM_NONCE_LEN], body[PKT_MAX_LEN];
    nonce_of(buf, nonce);
    if (!ccm_open(&g->ccm, nonce, buf, PKT_HEADER_LEN, &buf[PKT_HEADER_LEN], body_len,
                  &buf[PKT_HEADER_LEN + body_len], PKT_TAG_LEN, body)) {
        return PKT_FORGED;
    }
    memset(p, 0, sizeof(*p));
    p->group = buf[1];
    p->own.slot = buf[2] & 0x1F;
    p->session = (uint16_t)(buf[3] << 8 | buf[4]);
    p->sent = (uint32_t)buf[5] << 24 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 8 | buf[8];
    p->count = buf[9];
    p->own.has_pos = body[0] != AGE_NONE;
    p->own.age = body[0];
    get_where(&body[1], &p->own);
    p->speed_kmh = body[7];
    p->course2 = body[8];
    p->own.facts = body[9] & 0x0F;
    p->own.state = body[9] >> 4;
    p->beat = body[10] & 7;
    p->sats = body[10] >> 3 & 15;
    p->passengers = (uint8_t)((body_len - PKT_OWN_LEN) / PKT_PASSENGER_LEN);
    for (unsigned i = 0; i < p->passengers; i++) {
        const uint8_t *b = &body[PKT_OWN_LEN + i * PKT_PASSENGER_LEN];
        record_t *r = &p->passenger[i];
        r->slot = b[0] & 0x1F;
        r->has_pos = (b[0] & 0x20) != 0;
        get_where(&b[1], r);
        r->age = b[7];
        r->facts = b[8] & 0x0F;
        r->state = b[8] >> 4;
    }
    return PKT_OK;
}
