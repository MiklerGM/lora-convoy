#include "codec.h"

/*
 *  0     version (2 bits) | type (2 bits) | flags (4 bits, zero in v0)
 *  1     group
 *  2     slot
 *  3-5   counter, big-endian
 *  6-7   fix time, big-endian
 *  8-10  latitude, signed, 90 degrees = 2^23
 *  11-13 longitude, signed, 180 degrees = 2^23
 *  14    speed, km/h
 *  15    course, 2-degree steps
 *  16    status
 */

#define SCALE (1 << 23)

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

size_t pkt_encode(uint8_t out[PKT_POSITION_LEN], const position_t *p) {
    out[0] = (uint8_t)(PKT_VERSION << 6 | PKT_TYPE_POSITION << 4);
    out[1] = p->group;
    out[2] = p->slot;
    put24(&out[3], p->counter & 0xFFFFFFu);
    out[6] = (uint8_t)(p->fix_time >> 8);
    out[7] = (uint8_t)p->fix_time;
    put24(&out[8], (uint32_t)to24(p->lat_e7, 900000000ll) & 0xFFFFFFu);
    put24(&out[11], (uint32_t)to24(p->lon_e7, 1800000000ll) & 0xFFFFFFu);
    out[14] = p->speed_kmh;
    out[15] = p->course2;
    out[16] = p->status;
    return PKT_POSITION_LEN;
}

bool pkt_decode(const uint8_t *buf, size_t len, position_t *p) {
    if (len != PKT_POSITION_LEN || buf[0] >> 6 != PKT_VERSION || (buf[0] >> 4 & 3) != PKT_TYPE_POSITION) {
        return false;
    }
    p->group = buf[1];
    p->slot = buf[2];
    p->counter = get24(&buf[3]);
    p->fix_time = (uint16_t)(buf[6] << 8 | buf[7]);
    p->lat_e7 = from24(sign24(get24(&buf[8])), 900000000ll);
    p->lon_e7 = from24(sign24(get24(&buf[11])), 1800000000ll);
    p->speed_kmh = buf[14];
    p->course2 = buf[15];
    p->status = buf[16];
    return true;
}
