#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec.h"
#include "nmea.h"
#include "phy.h"

/*
 * The node core (ARCHITECTURE D7): everything the protocol decides, and nothing else. It reads no
 * clock and does no I/O; the platform hands it events stamped with its monotonic clock, in
 * microseconds, and sends what node_poll_tx gives it. The same code runs on both boards and in
 * the simulator.
 */

#define NODE_SLOTS 32

typedef struct {
    uint8_t group;
    uint8_t slot;            /* below NODE_SLOTS */
    uint32_t interval_ms;    /* mean time between our positions; each one is drawn +-20 % */
    uint16_t duty_permille;  /* the sub-band's duty cycle: 100 is 10 % an hour */
    phy_t phy;
} node_config_t;

typedef struct {
    bool known;
    position_t last;
    int64_t heard_us;        /* our clock when its last packet arrived */
    int64_t fix_utc_ms;      /* its fix in UTC, unwrapped against our time; 0 when we had none */
    int16_t rssi_dbm;
    int16_t snr_db4;         /* quarter dB */
    uint32_t received;       /* packets taken from it */
    uint32_t first_counter;  /* of the run of counters since it last started */
} rider_t;

/* Transmit time over the last hour, a minute at a time. */
typedef struct {
    uint32_t used_ms[60];
    int64_t minute[60];      /* which minute of our clock each bucket holds */
} duty_t;

typedef struct {
    node_config_t cfg;
    gnss_fix_t fix;          /* the last RMC, valid or not */
    int64_t fix_us;
    gnss_fix_t held;         /* the last valid position */
    bool have_held;
    bool have_time;          /* our UTC is time_utc_ms at time_us on our clock */
    int64_t time_utc_ms;
    int64_t time_us;
    uint32_t counter;
    int64_t next_tx_us;
    uint32_t rng;
    duty_t duty;
    rider_t riders[NODE_SLOTS];
    uint32_t rx_foreign;     /* decoded, but another group's */
    uint32_t rx_bad;         /* not a v0 position at all */
    uint32_t rx_clash;       /* our own slot, from another radio: a configuration error */
    uint32_t tx_skipped;     /* intervals that passed without a position to send, or duty left */
} node_t;

void node_init(node_t *n, const node_config_t *cfg, uint32_t seed, int64_t now_us);

void node_on_fix(node_t *n, const gnss_fix_t *fix, int64_t now_us);

/* True if the packet was ours to take: a v0 position of our group from another slot. */
bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us);

/* A packet to send now, its length returned, or 0. Its airtime is booked against the duty cycle
 * when it is handed over, so a send that then fails errs on the safe side. */
size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap);

/* Our UTC, from the last fix carried forward on our clock. False before the GNSS gave a time. */
bool node_utc_ms(const node_t *n, int64_t now_us, int64_t *utc_ms);

/* Transmit time over the last 60 minutes, in milliseconds. */
uint32_t node_duty_used_ms(const node_t *n, int64_t now_us);

/* One RIDER line, and the node's part of the status line: the phone API's words (api.h). */
int node_format_rider(const node_t *n, unsigned slot, int64_t now_us, char *buf, size_t cap);
int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap);
