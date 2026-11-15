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
    /* More lag for each satellite the fix used: the L76K's fix comes later the more it has to
     * reckon with, some 7 to 9 ms each (07, 2026-11-12). */
    int32_t nmea_lag_sat_us;
    /* From handing the radio a packet to its RF beginning: the radio's start-up, steady to 0.2 ms
     * on both boards (07, 2026-11-11). The packet is handed over this much before its slot. */
    int32_t tx_lead_us;
    /* Listen in our own slot instead of sending, once in this many frames at random; 0 never. Two
     * radios given one slot send together, and never hear each other otherwise (T-join). */
    uint16_t listen_one_in;
    /* How far this board's crystal may run from true time, in ppm, temperature included: what an
     * aging mark is taken to drift. 0 is 50. A T-Echo ran 102-115 ppm off (07, 2026-11-14). */
    uint16_t drift_ppm;
    /* When a board sends (12, section 2): its slot comes every frame, and it speaks in it once it
     * has moved this far from the position it last sent, has turned, has something changed to
     * tell, or has been silent for 2^beat frames. beat 0 sends in every frame. */
    uint16_t move_m;
    uint8_t beat;
    /* Records of other riders carried in each packet, 0 to PKT_PASSENGERS_MAX: the slot is sized
     * for this many. */
    uint8_t passengers;
    /* The bench's DEAF: slots whose own packets this board does not take, a bit each, as if they
     * were out of its reach; what others carry of them still comes. Not kept. */
    uint32_t deaf;
} node_config_t;

/* E0's measures (07, 09): the least, the sum and the most of what was measured, and how often. */
typedef struct {
    int64_t min_us, max_us, sum_us;
    uint32_t n;
} spread_t;

typedef struct {
    bool known;
    /* The latest told of it, by itself or by a rider that carried it: whichever says the later
     * as_of wins, so a record never gets younger on its way. */
    record_t rec;
    uint32_t as_of;          /* UTC seconds when it said so itself */
    uint32_t fix_utc;        /* UTC seconds of its fix, where it told us itself; 0 from a passenger */
    uint8_t via;             /* who told us: its own slot when itself */
    int64_t rec_us;          /* our clock when this record came */
    uint8_t speed_kmh, course2, beat, sats; /* from its own packet; unknown and 0 from a passenger */
    /* Heard itself: */
    bool heard;
    int64_t heard_us;        /* our clock when its last packet arrived */
    int16_t rssi_dbm;
    int16_t snr_db4;         /* quarter dB */
    uint16_t session;        /* of its last packet taken, with that packet's sent and count: a later */
    uint32_t sent;           /* packet must bring a later (sent, count) */
    uint8_t count;
    uint32_t received;       /* packets taken from it in this session */
    uint32_t expected;       /* and how many it sent meanwhile, by its count */
    spread_t offset;         /* E0: how late its packets began, from their end on our UTC, less the
                                airtime and their slot's moment */
    int64_t late_us;         /* and the last of them */
    /* Telling it on: */
    int64_t told_us;         /* our clock when we last carried it; 0 never */
    uint8_t news;            /* its state changed: our next packets carry it first, this many more */
} rider_t;

/* Transmit time over the last hour, a minute at a time. */
typedef struct {
    uint32_t used_ms[60];
    int64_t minute[60];      /* which minute of our clock each bucket holds */
} duty_t;

/* Better downwards. TIME_GROUP: set by the packets of riders whose slots are kept by PPS. */
typedef enum { TIME_NONE, TIME_NMEA, TIME_GROUP, TIME_PPS } time_source_t;

#define NODE_SYNC 5

typedef struct {
    node_config_t cfg;       /* its phy is the mode asked for ... */
    phy_t phy;               /* ... this the one the radio has taken, which the schedule, the ledger
                                and each packet's airtime reckon with (ARCHITECTURE D15) */
    gnss_fix_t fix;          /* the last RMC, valid or not */
    int64_t fix_us;
    gnss_fix_t held;         /* the last valid position */
    bool have_held;
    group_key_t group;       /* what the key gives: CCM key, tag, frame phases */
    uint16_t session;        /* ours, drawn at start and when our clock steps back */
    uint8_t count;           /* the next packet's */
    uint32_t last_sent;      /* the second our last packet named */
    uint32_t sent;           /* packets handed over to send since start */
    uint32_t rng;
    /* Our UTC: at mark_us on our clock it was mark_utc_us, give or take mark_err_us, and less sure
     * the longer ago that was (node_utc_us). */
    time_source_t time;
    int64_t mark_us;
    int64_t mark_utc_us;
    int64_t mark_err_us;
    int64_t pps_us;          /* the last PPS edge; 0 before one */
    /* A calendar from the phone, for the riders' ages only, and below any GNSS time: at phone_at_us
     * on our clock the phone's UTC was phone_utc_us, give or take phone_err_us. No slot opens on
     * it: the schedule wants GNSS time. */
    bool phone_time;
    int64_t phone_at_us, phone_utc_us, phone_err_us;
    uint32_t boot;           /* drawn at start: our clock's times belong to this run, for the phone */
    /* From a PPS edge to the NMEA fix of its second, where a board has both: E0's number. */
    int64_t lag_us, lag_min_us, lag_max_us;
    uint8_t state;           /* what our rider chose (the app's STATE) */
    /* Our own record seen in another's packet, with the state we are in: our packets get out, and
     * these slots carry what we say now, a bit each. News for the phone when a slot is new. */
    uint32_t echo_by;
    int64_t echo_us;         /* our clock at the last such; 0 before one */
    bool echo_news;
    uint32_t rx_changed;     /* the riders the packet taken last brought a newer record of, a bit
                                each, its sender's among them: their lines go to the phone at once */
    /* How late the packets of riders with PPS began by our clock, the latest NODE_SYNC of them:
     * their median is how far our clock is off (node_on_rx_done). */
    int64_t sync_us[NODE_SYNC];
    uint8_t sync_n, sync_at;
    /* What we last sent, for the send rule. */
    bool sent_any, sent_pos;
    int32_t sent_lat_e7, sent_lon_e7;
    uint8_t sent_course2, sent_told;
    int64_t sent_frame;
    uint8_t burst;           /* packets owed after a change of state or facts */
    bool stopped;            /* slower than walking for a minute */
    int64_t slow_since_us;   /* when it last went under 5 km/h; 0 while faster */
    int64_t last_frame;      /* the frame we last sent in, or passed over */
    int64_t tx_due_us;       /* our clock at the moment of the packet last handed over; 0 once done */
    uint32_t tx_air_us;      /* and its airtime, in the mode it went with */
    int rx_slot;             /* the slot of the packet last taken, until its RxDone is told; -1 none */
    size_t rx_len;
    spread_t tx_lead;        /* E0: from a packet's moment to its RF beginning, by the radio's TxDone */
    int64_t phase_frame;     /* the frame whose phase is in phase_us, for phase_spacing; -1 none */
    int64_t phase_us;
    int64_t phase_spacing;
    duty_t duty;
    rider_t riders[NODE_SLOTS];
    uint32_t rx_foreign;     /* another group's tag */
    uint32_t rx_bad;         /* not a packet of this version at all */
    uint32_t rx_forged;      /* our tag, and an authentication tag that does not check */
    uint32_t rx_replay;      /* genuine, but older than what we have: a repeat or a replay */
    uint32_t rx_clash;       /* our own slot, from another radio: a configuration error */
    uint32_t tx_skipped;     /* slots passed over for want of duty left */
    uint32_t tx_quiet;       /* slots passed over with nothing new to say */
    uint32_t tx_unsure;      /* slots passed over: our time was too uncertain to keep to them */
    uint32_t tx_late;        /* slots missed: we were asked after the moment had gone */
    uint32_t tx_listened;    /* slots spent listening for another radio in ours */
    uint32_t tx_held;        /* slots passed over while the radio had yet to take a mode asked for */
} node_t;

void node_init(node_t *n, const node_config_t *cfg, uint32_t seed);

/* An NMEA fix, at its arrival. With time in it, it also sets our clock: at the PPS edge before it,
 * if one came within the second, or at its arrival less the configured lag. */
void node_on_fix(node_t *n, const gnss_fix_t *fix, int64_t now_us);

/* A PPS edge, at our clock when it came. The fix that follows names its second. */
void node_on_pps(node_t *n, int64_t edge_us);

/* A group key, from the app: the riders heard under another key are forgotten. */
void node_set_key(node_t *n, const uint8_t key[16]);

/* The phone's UTC at at_us on our clock, give or take err_us (the phone API's TIME): the calendar
 * the riders' ages are told by while we have no GNSS time. It opens no slot. */
void node_set_phone_time(node_t *n, int64_t at_us, int64_t utc_us, int64_t err_us);

/* What our rider chose, 0 to STATES - 1. A change goes out in our next three slots, and who carries
 * it starts afresh. */
void node_set_state(node_t *n, uint8_t state);

/* True if the packet was ours to take: a genuine, fresh packet of our group from another slot. Its
 * passengers are taken with it, each if later than what we hold of that rider. */
bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us);

/* A packet to send now, its length returned, or 0: the radio's start-up (tx_lead_us) before our
 * slot, within TX_LATE_US of that moment, when the send rule finds something to say. out holds
 * PKT_MAX_LEN.
 * Its airtime is booked against the duty cycle when it is handed over, so a send that then fails
 * errs on the safe side. */
size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap);

/* The radio's RxDone, on our clock, as its interrupt stamped it, for the packet node_on_packet took
 * last: how late it began goes into its rider's offset= (E0). Without a stamp nothing is said, as
 * a poll's time is no measure of it. */
void node_on_rx_done(node_t *n, int64_t done_us);

/* The radio's TxDone, on our clock, for the packet node_poll_tx gave last: how late its RF began
 * after its moment, less the airtime, goes into the status (txlead=, E0). */
void node_on_tx_done(node_t *n, int64_t done_us);

/* The radio has taken this mode (radio_apply said so): from now the schedule, the ledger and the
 * airtime of what is sent reckon with it, and a node whose mode asked for is another sends nothing
 * until the radio has taken that one too. A node starts with its configured mode taken, which the
 * platform applies before it sends anything (run.c). */
void node_phy_applied(node_t *n, const phy_t *phy);

/* Our clock at the moment to hand over our next packet, the radio's start-up before its slot, for
 * the platform to be ready by; INT64_MAX while no
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
 * first, by category. One category so far. */
int node_format_ledger(const node_t *n, int64_t now_us, char *buf, size_t cap);

/* One RIDER line, and the node's part of the status line: the phone API's words (api.h). */
int node_format_rider(const node_t *n, unsigned slot, int64_t now_us, char *buf, size_t cap);
int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap);
