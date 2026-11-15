#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto.h"

/*
 * The air format, v5: what a rider says of itself, and short records of other riders it carries
 * along (passengers), under the group's key (docs/PROTOCOL.md). The header goes in clear and is
 * authenticated; the rest is encrypted; a 4-byte tag ends it (AES-128-CCM). 34 bytes with one
 * passenger. v3 was a position alone, 25 bytes, with calls for help answered packet by packet (D3).
 */

#define PKT_VERSION 5
#define PKT_TYPE_STATUS 0
#define PKT_HEADER_LEN 10
#define PKT_OWN_LEN 11
#define PKT_PASSENGER_LEN 9
#define PKT_TAG_LEN 4
#define PKT_PASSENGERS_MAX 3
#define PKT_LEN(passengers) (PKT_HEADER_LEN + PKT_OWN_LEN + (passengers) * PKT_PASSENGER_LEN + PKT_TAG_LEN)
#define PKT_MAX_LEN PKT_LEN(PKT_PASSENGERS_MAX)

#define SPEED_UNKNOWN 255
#define COURSE_UNKNOWN 255

/* Facts, observed by the board. */
#define FACT_STOPPED (1u << 0)
#define FACT_BATTERY_LOW (1u << 1)
#define FACT_TIME_PRECISE (1u << 2) /* its slots are kept by PPS: others may set their clocks by its packets */
#define FACT_CURRENT (1u << 3)      /* the position is the GNSS's current fix, not a held one */
/* The state its rider chose, 0-15; the app names them by activity. */
#define STATE_NORMAL 0
#define STATE_HELP 1
#define STATE_STOP_NOW 2
#define STATE_STOP_SOON 3 /* fuel, a snack, a rest: when convenient */
#define STATE_HELPING 4
#define STATES 16

/* An age in one byte: seconds below two minutes, then whole minutes to two hours. */
#define AGE_MINUTES_FROM 120
#define AGE_OLDER 254 /* beyond two hours */
#define AGE_NONE 255  /* its own record only: no position at all */
uint8_t age_code(uint32_t seconds);
uint32_t age_seconds(uint8_t code); /* the least it can mean */

/* What is told of one rider: by itself, or by another that carries it. */
typedef struct {
    uint8_t slot;
    bool has_pos;
    int32_t lat_e7; /* degrees x 1e7 */
    int32_t lon_e7;
    uint8_t age;    /* its own: the fix's age when sent. A passenger's: how long ago it said so itself */
    uint8_t facts;
    uint8_t state;
} record_t;

typedef struct {
    uint8_t group;     /* the group's tag: a byte of the key's own, so others' packets drop early */
    uint16_t session;  /* drawn at random at every start, and when the clock steps back */
    uint32_t sent;     /* UTC seconds when sent: in the nonce, and what every age counts from */
    uint8_t count;     /* packets of this session, modulo 256 */
    record_t own;      /* own.slot is the sender */
    uint8_t speed_kmh; /* 0..254, or SPEED_UNKNOWN */
    uint8_t course2;   /* course / 2 degrees, 0..179, or COURSE_UNKNOWN */
    uint8_t beat;      /* it sends at least every 2^beat frames, 0..7 */
    uint8_t sats;      /* the satellites its fix used, to 15 */
    uint8_t passengers;
    record_t passenger[PKT_PASSENGERS_MAX];
} packet_t;

/* What the 16-byte group key gives: the CCM key, the tag, and the key that draws each frame's
 * phase (node.c), each from the key by AES on a block of its own that starts with a byte no CCM
 * block can (bit 7 set), so none can stand in for another. */
typedef struct {
    uint8_t tag;
    uint32_t id;        /* the tag's block's first four bytes, the tag first: the group, to the phone */
    aes128_t ccm;
    aes128_t frame;
} group_key_t;

void group_key_init(group_key_t *g, const uint8_t key[16]);

/* Writes PKT_LEN(p->passengers) bytes into out, which holds PKT_MAX_LEN; p->group is ignored, the
 * key's tag goes out. Coordinates round to 1.07e-5 degrees of latitude and 2.15e-5 of longitude:
 * 1.2 m, and 2.4 m at the equator. */
size_t pkt_seal(uint8_t *out, const packet_t *p, const group_key_t *g);

typedef enum {
    PKT_OK,
    PKT_MALFORMED, /* not a packet of this version at all */
    PKT_FOREIGN,   /* another group's tag */
    PKT_FORGED,    /* our tag, and a CCM tag that does not check */
} pkt_result_t;

/* *p is written only for PKT_OK. */
pkt_result_t pkt_open(const uint8_t *buf, size_t len, const group_key_t *g, packet_t *p);
