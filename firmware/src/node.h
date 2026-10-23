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
    bool keyed;              /* without a key the board sends nothing: v1 has no clear text */
    uint8_t key[16];         /* the group's key, from the invitation (M5) */
    uint8_t slot;            /* ours, below slots */
    uint8_t slots;           /* riders in the group, 1 to NODE_SLOTS: a slot each in every frame */
    uint32_t interval_ms;    /* the frame: each rider sends once in it */
    uint16_t duty_permille;  /* the sub-band's duty cycle: 100 is 10 % an hour */
    phy_t phy;
    /* This board's GNSS without PPS: how long after its second an NMEA fix arrives, taken off its
     * arrival, and how far that may be off. The board's own measurement, until E0 gives one. */
    int32_t nmea_lag_us;
    int32_t nmea_err_us;
    /* Listen in our own slot instead of sending, once in this many frames at random; 0 never. Two
     * radios given one slot send together, and never hear each other otherwise (T-join). */
    uint16_t listen_one_in;
} node_config_t;

typedef struct {
    bool known;
    position_t last;
    int64_t heard_us;        /* our clock when its last packet arrived */
    int64_t fix_utc_ms;      /* its fix in UTC, unwrapped against our time; 0 when we had none */
    int16_t rssi_dbm;
    int16_t snr_db4;         /* quarter dB */
    uint32_t received;       /* packets taken from it in this session */
    uint16_t first_counter;  /* the session's first counter we took */
} rider_t;

/* Transmit time over the last hour, a minute at a time. */
typedef struct {
    uint32_t used_ms[60];
    int64_t minute[60];      /* which minute of our clock each bucket holds */
} duty_t;

typedef enum { TIME_NONE, TIME_NMEA, TIME_PPS } time_source_t;

typedef struct {
    node_config_t cfg;
    gnss_fix_t fix;          /* the last RMC, valid or not */
    int64_t fix_us;
    gnss_fix_t held;         /* the last valid position */
    bool have_held;
    group_key_t group;       /* what the key gives: CCM key, tag, frame phases */
    uint32_t session;        /* ours, drawn at start and when the counter wraps */
    uint16_t counter;        /* the next packet's, within the session */
    uint32_t sent;           /* packets handed over to send since start */
    uint32_t rng;
    /* Our UTC: at mark_us on our clock it was mark_utc_us, give or take mark_err_us, and less sure
     * the longer ago that was (node_utc_us). */
    time_source_t time;
    int64_t mark_us;
    int64_t mark_utc_us;
    int64_t mark_err_us;
    int64_t pps_us;          /* the last PPS edge; 0 before one */
    /* From a PPS edge to the NMEA fix of its second, where a board has both: E0's number. */
    int64_t lag_us, lag_min_us, lag_max_us;
    bool help;               /* our rider asks for help (the app's HELP) ... */
    uint32_t help_heard_by;  /* ... and these slots say they hear it, a bit each */
    uint8_t help_next;       /* where the search for a call for help to answer starts */
    bool stopped;            /* slower than walking for a minute */
    int64_t slow_since_us;   /* when it last went under 5 km/h; 0 while faster */
    int64_t last_frame;      /* the frame we last sent in, or passed over */
    int64_t phase_frame;     /* the frame whose phase is in phase_us, for phase_spacing; -1 none */
    int64_t phase_us;
    int64_t phase_spacing;
    duty_t duty;
    rider_t riders[NODE_SLOTS];
    uint32_t rx_foreign;     /* another group's tag */
    uint32_t rx_bad;         /* not a v1 position at all */
    uint32_t rx_forged;      /* our tag, and an authentication tag that does not check */
    uint32_t rx_replay;      /* genuine, but older than what we have: a repeat or a replay */
    uint32_t rx_clash;       /* our own slot, from another radio: a configuration error */
    uint32_t tx_skipped;     /* slots passed over without a position to send, or duty left */
    uint32_t tx_unsure;      /* slots passed over: our time was too uncertain to keep to them */
    uint32_t tx_late;        /* slots missed: we were asked after the moment had gone */
    uint32_t tx_listened;    /* slots spent listening for another radio in ours */
} node_t;

void node_init(node_t *n, const node_config_t *cfg, uint32_t seed);

/* An NMEA fix, at its arrival. With time in it, it also sets our clock: at the PPS edge before it,
 * if one came within the second, or at its arrival less the configured lag. */
void node_on_fix(node_t *n, const gnss_fix_t *fix, int64_t now_us);

/* A PPS edge, at our clock when it came. The fix that follows names its second. */
void node_on_pps(node_t *n, int64_t edge_us);

/* A group key, from the app: the riders heard under another key are forgotten. */
void node_set_key(node_t *n, const uint8_t key[16]);

/* Our rider's call for help, on or off. Each time it goes on, who has heard it starts afresh. */
void node_set_help(node_t *n, bool on);

/* True if the packet was ours to take: a genuine, fresh v1 position of our group from another slot. */
bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us);

/* A packet to send now, its length returned, or 0: at our slot, within TX_LATE_US of its moment.
 * Its airtime is booked against the duty cycle when it is handed over, so a send that then fails
 * errs on the safe side. */
size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap);

/* Our clock at the moment of our next slot, for the platform to be ready by; INT64_MAX while no
 * slot can be told (no key, no time, a slot outside the group). Due already if not above now_us. */
int64_t node_next_tx_us(const node_t *n, int64_t now_us);

/* Our UTC in microseconds, and how far it may be off. False before the GNSS gave a time. */
bool node_utc_us(const node_t *n, int64_t now_us, int64_t *utc_us, int64_t *err_us);
bool node_utc_ms(const node_t *n, int64_t now_us, int64_t *utc_ms);

/* How far our time may be off and still send: the gap between two slots, less what the receivers
 * need, shared by the two senders either side of it. 0 when the slots do not fit the frame. */
int64_t node_time_budget_us(const node_t *n);

/* Transmit time over the last 60 minutes, in milliseconds. */
uint32_t node_duty_used_ms(const node_t *n, int64_t now_us);

/* The duty ledger (01, requirements): transmit time a minute at a time over the last hour, oldest
 * first, by category. Positions are the only category so far. */
int node_format_ledger(const node_t *n, int64_t now_us, char *buf, size_t cap);

/* One RIDER line, and the node's part of the status line: the phone API's words (api.h). */
int node_format_rider(const node_t *n, unsigned slot, int64_t now_us, char *buf, size_t cap);
int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap);
