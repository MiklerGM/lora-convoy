#include "node.h"

#include <stdio.h>
#include <string.h>

/* How old our own fix may be and still count as the GNSS's current one. */
#define FIX_FRESH_US (3 * 1000000ll)

/* The schedule's terms (docs/PROTOCOL.md, When a board sends). Bounds, not measurements: E0 measures
 * each board. */
#define DRIFT_PPM 50       /* our crystal against true time, temperature included */
#define PPS_ERR_US 100     /* a PPS edge's timestamp: the GNSS's own error and the interrupt's latency */
/* An NMEA fix this soon after a PPS edge is that edge's second. ponytail: a fix that came after the
 * next edge would be paired a second out; the L76K's RMC comes some 100 ms after its edge, and lag=
 * shows on the bench if it ever nears a second. An edge count from the interrupt settles it. */
#define PPS_PAIR_US 900000
#define TX_ERR_US 1000     /* from the moment to the air: the platform's wake-up and the radio's start */
#define TX_LATE_US 2000    /* a slot later than this is passed over, never sent late */
#define RX_GAP_US 2000     /* between two packets, for a receiver to read the first one out */

/* Stopped: under 5 km/h for a minute, so a red light is not a stop; moving again over 10 km/h, so
 * a GNSS's wander at a standstill does not start it up. */
#define STOP_BELOW_KMH10 50
#define MOVING_OVER_KMH10 100
#define STOP_AFTER_US (60 * 1000000ll)
/* A call for help heard this recently is answered: one heard longer ago may have been called off
 * out of our range. */
#define HELP_FRESH_US (60 * 1000000ll)

static uint32_t next_random(node_t *n) {
    /* xorshift32: sessions only need to differ, the key keeps them secret */
    uint32_t x = n->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return n->rng = x;
}

void node_init(node_t *n, const node_config_t *cfg, uint32_t seed) {
    memset(n, 0, sizeof(*n));
    n->cfg = *cfg;
    n->rng = seed ? seed : 0x9E3779B9u;
    /* The platform's seed is from the chip's own generator, so every start draws a new session. */
    n->session = next_random(n);
    /* Calls are numbered on from where the session puts them, so a call after a restart seldom
     * takes the number of the one before it, which others may still be answering. */
    n->help_call = (uint8_t)(n->session % HELP_CALLS);
    if (cfg->keyed) group_key_init(&n->group, cfg->key);
    n->last_frame = n->phase_frame = -1;
}

void node_set_key(node_t *n, const uint8_t key[16]) {
    /* the key it has, sent again by a phone that reconnects: the riders stay */
    if (n->cfg.keyed && memcmp(n->cfg.key, key, 16) == 0) return;
    memcpy(n->cfg.key, key, 16);
    n->cfg.keyed = true;
    group_key_init(&n->group, key);
    n->phase_frame = -1;
    memset(n->riders, 0, sizeof(n->riders));
    n->help_heard_by = 0;
}

void node_set_help(node_t *n, bool on) {
    if (on && !n->help) {
        n->help_call = (uint8_t)(n->help_call % HELP_CALLS + 1);
        n->help_heard_by = 0;
    }
    n->help = on;
}

/* ---- time ------------------------------------------------------------------------ */

bool node_utc_us(const node_t *n, int64_t now_us, int64_t *utc_us, int64_t *err_us) {
    if (n->time == TIME_NONE) return false;
    const int64_t since = now_us - n->mark_us;
    *utc_us = n->mark_utc_us + since;
    *err_us = n->mark_err_us + (since < 0 ? -since : since) * DRIFT_PPM / 1000000;
    return true;
}

bool node_utc_ms(const node_t *n, int64_t now_us, int64_t *utc_ms) {
    int64_t utc, err;
    if (!node_utc_us(n, now_us, &utc, &err)) return false;
    *utc_ms = utc / 1000;
    return true;
}

/* A new mark for our clock, taken unless the one we have, carried to now, is still better. */
static void set_time(node_t *n, time_source_t source, int64_t at_us, int64_t utc_us, int64_t err_us,
                     int64_t now_us) {
    int64_t utc, err;
    const int64_t carried = err_us + (now_us - at_us) * DRIFT_PPM / 1000000;
    if (node_utc_us(n, now_us, &utc, &err) && err < carried) return;
    /* The frame running when the time first comes is not ours: its slot may have gone already. */
    if (n->time == TIME_NONE) n->last_frame = utc_us / ((int64_t)n->cfg.interval_ms * 1000);
    n->time = source;
    n->mark_us = at_us;
    n->mark_utc_us = utc_us;
    n->mark_err_us = err_us;
}

void node_on_pps(node_t *n, int64_t edge_us) { n->pps_us = edge_us; }

void node_on_fix(node_t *n, const gnss_fix_t *fix, int64_t now_us) {
    n->fix = *fix;
    n->fix_us = now_us;
    if (fix->time_valid) {
        const int64_t utc_us = fix->utc_ms * 1000;
        const int64_t lag = now_us - n->pps_us;
        if (n->pps_us && lag > 0 && lag < PPS_PAIR_US && fix->utc_ms % 1000 == 0) {
            /* The edge was this fix's second. How far the NMEA lags behind it is E0's number for
             * boards without PPS, measured here for free. */
            if (!n->lag_us || lag < n->lag_min_us) n->lag_min_us = lag;
            if (!n->lag_us || lag > n->lag_max_us) n->lag_max_us = lag;
            n->lag_us = lag;
            set_time(n, TIME_PPS, n->pps_us, utc_us, PPS_ERR_US, now_us);
        } else {
            set_time(n, TIME_NMEA, now_us - n->cfg.nmea_lag_us, utc_us, n->cfg.nmea_err_us, now_us);
        }
    }
    if (fix->valid) {
        n->held = *fix;
        n->have_held = true;
        if (fix->speed_kmh10 >= MOVING_OVER_KMH10) {
            n->stopped = false;
            n->slow_since_us = 0;
        } else if (fix->speed_kmh10 >= 0 && fix->speed_kmh10 < STOP_BELOW_KMH10) {
            if (!n->slow_since_us) n->slow_since_us = now_us;
            if (now_us - n->slow_since_us >= STOP_AFTER_US) n->stopped = true;
        } else if (!n->stopped) {
            n->slow_since_us = 0; /* between the two: rolling, neither stopping nor stopped */
        }
    }
}

/* ---- the duty cycle ---------------------------------------------------------- */

static void duty_book(duty_t *d, int64_t now_us, uint32_t ms) {
    const int64_t minute = now_us / 60000000;
    const int b = (int)(minute % 60);
    if (d->minute[b] != minute) {
        d->minute[b] = minute;
        d->used_ms[b] = 0;
    }
    d->used_ms[b] += ms;
}

uint32_t node_duty_used_ms(const node_t *n, int64_t now_us) {
    const int64_t minute = now_us / 60000000;
    uint32_t sum = 0;
    for (int b = 0; b < 60; b++) {
        /* The current minute and the 59 before it: an hour, give or take the minute running. */
        if (n->duty.used_ms[b] && minute - n->duty.minute[b] < 60) sum += n->duty.used_ms[b];
    }
    return sum;
}

int node_format_ledger(const node_t *n, int64_t now_us, char *buf, size_t cap) {
    const int64_t minute = now_us / 60000000;
    int len = snprintf(buf, cap, "LEDGER minutes=60 limit_ms=%lu used_ms=%lu positions_ms=",
                       (unsigned long)(3600u * n->cfg.duty_permille), (unsigned long)node_duty_used_ms(n, now_us));
    for (int64_t m = minute - 59; m <= minute && len > 0 && (size_t)len < cap; m++) {
        const int b = (int)(((m % 60) + 60) % 60);
        const uint32_t ms = m >= 0 && n->duty.minute[b] == m ? n->duty.used_ms[b] : 0;
        len += snprintf(buf + len, cap - (size_t)len, m < minute ? "%lu," : "%lu", (unsigned long)ms);
    }
    return len;
}

/* ---- the schedule ---------------------------------------------------------------- */

/* Frames are numbered from 1970 in UTC: frame k starts at k intervals. A frame holds a slot per
 * rider and a spare one, evenly spaced; our slot s comes at the frame's start, plus its phase,
 * plus s spacings. */
static int64_t frame_us(const node_t *n) { return (int64_t)n->cfg.interval_ms * 1000; }

static int64_t spacing_us(const node_t *n) { return frame_us(n) / (n->cfg.slots + 1); }

int64_t node_time_budget_us(const node_t *n) {
    const int64_t gap = spacing_us(n) - phy_airtime_us(&n->cfg.phy, PKT_POSITION_LEN) - RX_GAP_US;
    return gap > 0 ? gap / 2 : 0;
}

/* The frame's phase, under one spacing: the same for the whole group, drawn anew for each frame
 * from the key. Two groups on one channel then meet in a frame now and then, never in every one
 * (T-neighbours); the spare slot keeps the last packet of a frame a spacing clear of the first of
 * the next, whatever their phases. */
static int64_t phase_us(const node_t *n, int64_t k) {
    uint8_t in[16] = {0}, out[16];
    for (int i = 0; i < 8; i++) in[15 - i] = (uint8_t)((uint64_t)k >> (8 * i));
    aes128_encrypt(&n->group.frame, in, out);
    const uint32_t r = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 | out[3];
    return (int64_t)(r % (uint64_t)spacing_us(n));
}

static int64_t slot_utc_us(const node_t *n, int64_t k, int64_t phase) {
    return k * frame_us(n) + phase + (int64_t)n->cfg.slot * spacing_us(n);
}

/* The same, with the phase kept for the frame: one AES per frame, not per poll. */
static int64_t our_slot(node_t *n, int64_t k) {
    if (k != n->phase_frame || n->phase_spacing != spacing_us(n)) {
        n->phase_us = phase_us(n, k);
        n->phase_frame = k;
        n->phase_spacing = spacing_us(n);
    }
    return slot_utc_us(n, k, n->phase_us);
}

/* ---- sending ------------------------------------------------------------------- */

/* A rider calling for help whom we hear, to answer in our packet, with the number of the call we
 * heard: each in turn, when several call. */
static void help_to_answer(node_t *n, int64_t now_us, position_t *p) {
    p->help_heard = HELP_HEARD_NONE;
    for (int i = 0; i < NODE_SLOTS; i++) {
        const int s = (n->help_next + i) % NODE_SLOTS;
        const rider_t *r = &n->riders[s];
        if (r->known && (r->last.status & ST_HELP) && now_us - r->heard_us <= HELP_FRESH_US) {
            n->help_next = (uint8_t)((s + 1) % NODE_SLOTS);
            p->help_heard = (uint8_t)s;
            p->heard_call = r->last.call;
            return;
        }
    }
}

size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap) {
    int64_t utc, err;
    /* Nothing without a time to find our slot by. */
    if (!node_utc_us(n, now_us, &utc, &err)) return 0;
    const int64_t k = utc / frame_us(n);
    /* Nothing at all without a key (v1 has no clear text), nor from a slot outside the group; the
     * frames go by meanwhile, so the one running when the key comes is not counted as missed. */
    if (!n->cfg.keyed || n->cfg.slot >= n->cfg.slots) {
        n->last_frame = k;
        return 0;
    }
    /* Frames only go forward: a time set back a little must not send twice. Set back further, it
     * was the time before that was wrong. */
    if (k < n->last_frame - 1) n->last_frame = -1;
    if (k <= n->last_frame) return 0;
    const int64_t at = our_slot(n, k);
    if (utc < at) return 0;
    n->last_frame = k;
    if (utc - at > TX_LATE_US) {
        n->tx_late++;
        return 0;
    }
    /* Too unsure of the time, the packet could land on a neighbour's slot. */
    if (err + TX_ERR_US > node_time_budget_us(n)) {
        n->tx_unsure++;
        return 0;
    }
    /* Now and then our slot is for listening: another radio in it is heard then, as a clash. */
    if (n->cfg.listen_one_in && next_random(n) % n->cfg.listen_one_in == 0) {
        n->tx_listened++;
        return 0;
    }
    /* Nothing true to say before the first fix. */
    const uint32_t air_ms = (phy_airtime_us(&n->cfg.phy, PKT_POSITION_LEN) + 999) / 1000;
    const uint32_t limit_ms = 3600u * n->cfg.duty_permille;
    if (!n->have_held || cap < PKT_POSITION_LEN || node_duty_used_ms(n, now_us) + air_ms > limit_ms) {
        n->tx_skipped++;
        return 0;
    }
    const gnss_fix_t *h = &n->held;
    /* The held position goes out with its own time, so it ages honestly; GPS_OK says whether it
     * is also the GNSS's current fix. */
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    position_t p = {
        .slot = n->cfg.slot,
        .session = n->session,
        .counter = n->counter,
        .fix_time = (uint16_t)((h->utc_ms / 1000) & 0xFFFF),
        .lat_e7 = h->lat_e7,
        .lon_e7 = h->lon_e7,
        .speed_kmh = h->speed_kmh10 < 0 ? SPEED_UNKNOWN
                     : h->speed_kmh10 >= 2540 ? 254 : (uint8_t)((h->speed_kmh10 + 5) / 10),
        .course2 = h->course_deg10 < 0 ? COURSE_UNKNOWN : (uint8_t)(((h->course_deg10 + 10) / 20) % 180),
        .status = (uint8_t)((current ? ST_GPS_OK : 0) | (n->help ? ST_HELP : 0) | (n->stopped ? ST_STOPPED : 0)),
        .call = n->help ? n->help_call : 0,
    };
    help_to_answer(n, now_us, &p);
    /* The nonce must never repeat: before the counter wraps, a new session. */
    if (++n->counter == 0) n->session = next_random(n);
    n->sent++;
    duty_book(&n->duty, now_us, air_ms);
    return pkt_seal(out, &p, &n->group);
}

int64_t node_next_tx_us(const node_t *n, int64_t now_us) {
    int64_t utc, err;
    if (!n->cfg.keyed || n->cfg.slot >= n->cfg.slots || !node_utc_us(n, now_us, &utc, &err)) return INT64_MAX;
    const int64_t k = utc / frame_us(n);
    int64_t at = slot_utc_us(n, k, phase_us(n, k));
    if (k <= n->last_frame || utc - at > TX_LATE_US) at = slot_utc_us(n, k + 1, phase_us(n, k + 1));
    return now_us + (at - utc);
}

/* ---- receiving ------------------------------------------------------------------- */

/* How far back in time a new session's first fix may lie behind what we have and still be taken:
 * a rider's own clock and ours disagree by a second or so. */
#define REPLAY_SLACK_MS 2000

/* Where a session is among a rider's latest, or -1. */
static int session_seen(const rider_t *r, uint32_t session) {
    for (int i = 0; i < r->sessions_seen; i++) {
        if (r->session_seen[i] == session) return i;
    }
    return -1;
}

/* The highest counter taken in a session; a session not among them goes in first, and the oldest
 * of them goes. */
static void remember_session(rider_t *r, uint32_t session, uint16_t counter) {
    int i = session_seen(r, session);
    if (i < 0) {
        const int keep = r->sessions_seen < NODE_SESSIONS ? r->sessions_seen++ : NODE_SESSIONS - 1;
        memmove(&r->session_seen[1], &r->session_seen[0], (size_t)keep * sizeof(r->session_seen[0]));
        memmove(&r->counter_seen[1], &r->counter_seen[0], (size_t)keep * sizeof(r->counter_seen[0]));
        r->session_seen[0] = session;
        i = 0;
    }
    r->counter_seen[i] = counter;
}

bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us) {
    position_t p;
    if (!n->cfg.keyed) {
        n->rx_foreign++;
        return false;
    }
    switch (pkt_open(buf, len, &n->group, &p)) {
        case PKT_OK: break;
        case PKT_FOREIGN: n->rx_foreign++; return false;
        case PKT_FORGED: n->rx_forged++; return false;
        default: n->rx_bad++; return false;
    }
    if (p.slot >= NODE_SLOTS) {
        n->rx_bad++;
        return false;
    }
    if (p.slot == n->cfg.slot) {
        /* Two radios on one slot: counted, not hidden. Their sessions keep the nonces apart. */
        n->rx_clash++;
        return false;
    }
    int64_t fix_utc_ms = 0;
    int64_t utc;
    if (node_utc_ms(n, now_us, &utc)) {
        /* The fix time is UTC seconds modulo 65536: unwrapped to the nearest, past or future. A
         * future one is a clock gone wrong, kept as such rather than read as fresh. */
        const int64_t now_s = utc / 1000;
        const uint16_t behind = (uint16_t)((uint16_t)now_s - p.fix_time);
        fix_utc_ms = (behind < 0x8000 ? now_s - behind : now_s + (0x10000 - behind)) * 1000;
    }
    rider_t *r = &n->riders[p.slot];
    if (r->known) {
        /* A repeat or a replay: in each of its latest sessions the counter only grows, whichever
         * was heard last; and a packet of any session but the last must bring a fix no older than
         * the one we hold, or it is an old one played again. ponytail: four sessions a rider, so a
         * replay cycling through more of them, recorded close in time, gets through; the frame
         * number in the nonce would end it (T-nonce). */
        const int seen = session_seen(r, p.session);
        if ((seen >= 0 && p.counter <= r->counter_seen[seen]) ||
            (p.session != r->last.session && fix_utc_ms && r->fix_utc_ms &&
             fix_utc_ms < r->fix_utc_ms - REPLAY_SLACK_MS)) {
            n->rx_replay++;
            return false;
        }
        if (p.session != r->last.session) r->received = 0;
    }
    remember_session(r, p.session, p.counter);
    /* Another call, or none: who answered the one before has not answered this one. */
    if (r->known && p.call != r->last.call) r->answered_by = 0;
    if (!r->known || p.session != r->last.session) r->first_counter = p.counter;
    r->received++;
    r->known = true;
    r->last = p;
    r->heard_us = now_us;
    r->rssi_dbm = (int16_t)rssi_dbm;
    r->snr_db4 = (int16_t)snr_db4;
    r->fix_utc_ms = fix_utc_ms;
    /* It hears a call for help: ours, or another rider's, and the call it names, not one before.
     * ponytail: an answer counts for the rest of the call, however long ago it came; the answerer's
     * own heard= tells how long since we heard it at all. */
    if (p.help_heard == n->cfg.slot) {
        if (n->help && p.heard_call == n->help_call) n->help_heard_by |= 1u << p.slot;
    } else if (p.help_heard < NODE_SLOTS) {
        rider_t *caller = &n->riders[p.help_heard];
        if (caller->known && (caller->last.status & ST_HELP) && caller->last.call == p.heard_call) {
            caller->answered_by |= 1u << p.slot;
        }
    }
    return true;
}

/* ---- lines ------------------------------------------------------------------------- */

/* Degrees x 1e7 as text, sign included: -0.5 must not print as 0.5. */
static const char *deg(char *s, size_t cap, int32_t e7) {
    const long long v = e7;
    const long long a = v < 0 ? -v : v;
    snprintf(s, cap, "%s%lld.%07lld", v < 0 ? "-" : "", a / 10000000, a % 10000000);
    return s;
}

int node_format_rider(const node_t *n, unsigned slot, int64_t now_us, char *buf, size_t cap) {
    if (slot >= NODE_SLOTS || !n->riders[slot].known) return snprintf(buf, cap, "RIDER slot=%u none", slot);
    const rider_t *r = &n->riders[slot];
    char lat[16], lon[16], age[24];
    int64_t utc;
    if (r->fix_utc_ms && node_utc_ms(n, now_us, &utc)) {
        snprintf(age, sizeof(age), "%lld", (long long)((utc - r->fix_utc_ms) / 1000));
    } else {
        snprintf(age, sizeof(age), "?");
    }
    char speed[8] = "?", course[8] = "?", answered[96] = "-";
    for (int s = 0, at = 0; s < NODE_SLOTS; s++) {
        if (r->answered_by >> s & 1) at += snprintf(answered + at, sizeof(answered) - at, "%s%d", at ? "," : "", s);
    }
    if (r->last.speed_kmh != SPEED_UNKNOWN) snprintf(speed, sizeof(speed), "%u", r->last.speed_kmh);
    if (r->last.course2 != COURSE_UNKNOWN) snprintf(course, sizeof(course), "%u", r->last.course2 * 2u);
    const long long heard10 = (now_us - r->heard_us) / 100000;
    const int snr = r->snr_db4 < 0 ? -r->snr_db4 : r->snr_db4;
    return snprintf(buf, cap,
                    "RIDER slot=%u lat=%s lon=%s fix_age=%s heard=%lld.%lld speed=%s course=%s gps=%d "
                    "help=%d stopped=%d answered=%s rssi=%d snr=%s%d.%02d rx=%lu/%lu",
                    slot, deg(lat, sizeof(lat), r->last.lat_e7), deg(lon, sizeof(lon), r->last.lon_e7),
                    age, heard10 / 10, heard10 % 10, speed, course,
                    (r->last.status & ST_GPS_OK) ? 1 : 0, (r->last.status & ST_HELP) ? 1 : 0,
                    (r->last.status & ST_STOPPED) ? 1 : 0, answered, r->rssi_dbm, r->snr_db4 < 0 ? "-" : "",
                    snr / 4, snr % 4 * 25, (unsigned long)r->received,
                    (unsigned long)((uint16_t)(r->last.counter - r->first_counter) + 1u));
}

int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap) {
    const uint32_t used = node_duty_used_ms(n, now_us);
    int known = 0;
    for (int i = 0; i < NODE_SLOTS; i++) known += n->riders[i].known;
    char lat[16], lon[16], err_ms[48] = "?", lag[48] = "?";
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    static const char *const SOURCE[] = {"none", "nmea", "pps"};
    int64_t utc, err;
    if (node_utc_us(n, now_us, &utc, &err)) {
        snprintf(err_ms, sizeof(err_ms), "%lld.%lld", (long long)(err / 1000), (long long)(err % 1000 / 100));
    }
    if (n->lag_us) {
        /* under PPS_PAIR_US, all three */
        snprintf(lag, sizeof(lag), "%d/%d/%d", (int)(n->lag_us / 1000), (int)(n->lag_min_us / 1000),
                 (int)(n->lag_max_us / 1000));
    }
    const int64_t budget = node_time_budget_us(n);
    return snprintf(buf, cap,
                    "slot=%u slots=%u keyed=%d group=%02x mode=%u/%lu fix=%d stopped=%d help=%d heard=%d lat=%s lon=%s sats=%u "
                    "hdop=%u.%u time=%s err=%s "
                    "budget=%lld.%lld lag=%s tx=%lu late=%lu unsure=%lu skipped=%lu duty=%lu.%02lu%% riders=%d "
                    "bad=%lu foreign=%lu forged=%lu replay=%lu clash=%lu",
                    n->cfg.slot, n->cfg.slots, n->cfg.keyed ? 1 : 0, n->cfg.keyed ? n->group.tag : 0, n->cfg.phy.sf,
                    (unsigned long)(n->cfg.phy.bw_hz / 1000),
                    current ? 1 : 0, n->stopped ? 1 : 0, n->help ? 1 : 0, __builtin_popcount(n->help_heard_by),
                    deg(lat, sizeof(lat), n->held.lat_e7),
                    deg(lon, sizeof(lon), n->held.lon_e7), n->fix.sats, n->fix.hdop10 / 10,
                    n->fix.hdop10 % 10, SOURCE[n->time], err_ms, (long long)(budget / 1000),
                    (long long)(budget % 1000 / 100), lag, (unsigned long)n->sent, (unsigned long)n->tx_late,
                    (unsigned long)n->tx_unsure, (unsigned long)n->tx_skipped, (unsigned long)(used / 36000),
                    (unsigned long)(used % 36000 / 360), known, (unsigned long)n->rx_bad,
                    (unsigned long)n->rx_foreign, (unsigned long)n->rx_forged, (unsigned long)n->rx_replay,
                    (unsigned long)n->rx_clash);
}
