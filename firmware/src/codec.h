#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The air format, v0: one packet type, a rider's own position, 17 bytes in clear. Bench only:
 * nothing is ridden before authenticated encryption (docs/PROTOCOL.md, ARCHITECTURE D11).
 */

#define PKT_VERSION 0
#define PKT_TYPE_POSITION 0
#define PKT_POSITION_LEN 17

#define SPEED_UNKNOWN 255
#define COURSE_UNKNOWN 255

/* Status bits. */
#define ST_HELP (1u << 0)
#define ST_STOPPED (1u << 1)
#define ST_GPS_OK (1u << 2)       /* the position is the GNSS's current fix, not a held one */
#define ST_BATTERY_LOW (1u << 3)

typedef struct {
    uint8_t group;      /* which group: v0 sets it by hand, v1 derives it from the key */
    uint8_t slot;       /* which rider */
    uint32_t counter;   /* 24 bits, per sender, from 0 at every start */
    uint16_t fix_time;  /* UTC seconds of the fix, modulo 65536 */
    int32_t lat_e7;     /* degrees x 1e7 */
    int32_t lon_e7;
    uint8_t speed_kmh;  /* 0..254, or SPEED_UNKNOWN */
    uint8_t course2;    /* course / 2 degrees, 0..179, or COURSE_UNKNOWN */
    uint8_t status;
} position_t;

/* Writes PKT_POSITION_LEN bytes. Coordinates round to 1.07e-5 degrees of latitude and 2.15e-5 of
 * longitude: 1.2 m north-south, 2.4 m east-west at the equator. */
size_t pkt_encode(uint8_t out[PKT_POSITION_LEN], const position_t *p);

/* False for anything that is not a v0 position packet, without touching *p. */
bool pkt_decode(const uint8_t *buf, size_t len, position_t *p);
