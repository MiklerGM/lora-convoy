#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

/*
 * The air format, v3: a rider's own position, 25 bytes, under the group's key (docs/PROTOCOL.md).
 * The header goes in clear and is authenticated; the position is encrypted; a 4-byte tag ends it
 * (AES-128-CCM). v1 was the same without the help heard, v2 without the calls' numbers (D3).
 */

#define PKT_VERSION 3
#define PKT_TYPE_POSITION 0
#define PKT_HEADER_LEN 9
#define PKT_BODY_LEN 12
#define PKT_TAG_LEN 4
#define PKT_POSITION_LEN (PKT_HEADER_LEN + PKT_BODY_LEN + PKT_TAG_LEN)

#define SPEED_UNKNOWN 255
#define COURSE_UNKNOWN 255
#define HELP_HEARD_NONE 255

/* Status bits. */
#define ST_HELP (1u << 0)
#define ST_STOPPED (1u << 1)
#define ST_GPS_OK (1u << 2)       /* the position is the GNSS's current fix, not a held one */
#define ST_BATTERY_LOW (1u << 3)
/* A call for help's number, 1-7, which a new call moves on: an answer names the call it hears. */
#define HELP_CALLS 7

typedef struct {
    uint8_t group;      /* the group's tag: a byte of the key's own, so others' packets drop early */
    uint8_t slot;       /* which rider */
    uint32_t session;   /* drawn at random at every start: with the counter, the nonce never repeats */
    uint16_t counter;   /* per session; a new session before it wraps */
    uint16_t fix_time;  /* UTC seconds of the fix, modulo 65536 */
    int32_t lat_e7;     /* degrees x 1e7 */
    int32_t lon_e7;
    uint8_t speed_kmh;  /* 0..254, or SPEED_UNKNOWN */
    uint8_t course2;    /* course / 2 degrees, 0..179, or COURSE_UNKNOWN */
    uint8_t status;
    uint8_t call;       /* with ST_HELP, our call's number, 1..HELP_CALLS; 0 without */
    uint8_t help_heard; /* the slot of a rider whose call for help we hear, or HELP_HEARD_NONE */
    uint8_t heard_call; /* and that call's number */
} position_t;

/* What the 16-byte group key gives: the CCM key, the tag, and the key that draws each frame's
 * phase (node.c), each from the key by AES on a block of its own that starts with a byte no CCM
 * block can (bit 7 set), so none can stand in for another. */
typedef struct {
    uint8_t tag;
    aes128_t ccm;
    aes128_t frame;
} group_key_t;

void group_key_init(group_key_t *g, const uint8_t key[16]);

/* Writes PKT_POSITION_LEN bytes; p->group is ignored, the key's tag goes out. Coordinates round to
 * 1.07e-5 degrees of latitude and 2.15e-5 of longitude: 1.2 m, and 2.4 m at the equator. */
size_t pkt_seal(uint8_t out[PKT_POSITION_LEN], const position_t *p, const group_key_t *g);

typedef enum {
    PKT_OK,
    PKT_MALFORMED, /* not a position of this version at all */
    PKT_FOREIGN,   /* another group's tag */
    PKT_FORGED,    /* our tag, and a CCM tag that does not check */
} pkt_result_t;

/* *p is written only for PKT_OK. */
pkt_result_t pkt_open(const uint8_t *buf, size_t len, const group_key_t *g, position_t *p);
