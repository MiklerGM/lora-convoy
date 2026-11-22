#include "node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The schedule's terms (docs/PROTOCOL.md, When a board sends). Bounds, not measurements: E0 measures
 * each board. */
#define DRIFT_PPM 50       /* our crystal against true time, temperature included, unless the board says */
#define PPS_ERR_US 100     /* a PPS edge's timestamp: the GNSS's own error and the interrupt's latency */
/* An NMEA fix this soon after a PPS edge is that edge's second. ponytail: a fix that came after the
 * next edge would be paired a second out; the L76K's RMC comes some 100 ms after its edge, and lag=
 * shows on the bench if it ever nears a second. An edge count from the interrupt settles it. */
#define PPS_PAIR_US 900000
#define TX_ERR_US 1000     /* from the moment to the air: the platform's wake-up and the radio's start */
#define TX_LATE_US 2000    /* a slot later than this is passed over, never sent late */
#define RX_GAP_US 2000     /* between two packets, for a receiver to read the first one out */

/* Stopped: under 5 km/h and within 30 m for a minute, so a red light is not a stop and a walker at
 * 4 km/h, 30 m on in under half a minute, is not stopped either; moving again over 10 km/h or 30 m
 * from where its minute began, which a GNSS's wander at a standstill seldom is. */
#define STOP_BELOW_KMH10 50
#define MOVING_OVER_KMH10 100
#define STOP_AFTER_US (60 * 1000000ll)
#define STOP_WITHIN_M 30
static bool apart(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, uint32_t metres);
/* Time from the group (12, 2026-11-14): a rider on PPS no further off than this says "precise" ... */
#define PRECISE_ERR_US 5000
#define GROUP_ERR_US 10000 /* ... and a clock set by such packets is good to this */
#define SYNC_MAX_US 300000 /* a packet further from its slot than this sets nothing */
#define SYNC_MIN 3         /* packets before the first setting */
/* A packet whose sent lies this far from our time is a recording played back, or a clock gone wrong. */
#define SENT_SLACK_S 3
#define RECORD_TTL_S 600   /* a record older than this is not carried on */
#define NEWS_TIMES 3       /* packets a change is told in, so that one loss does not hide it */
#define TURN_DEG2 15       /* a turn of 30 degrees ... */
#define TURN_AFTER_M 5     /* ... once this far from the position last sent */

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
    n->phy = cfg->phy;
    n->rng = seed ? seed : 0x9E3779B9u;
    /* The platform's seed is from the chip's own generator, so every start draws a new session. */
    n->boot = next_random(n);
    n->session = (uint16_t)n->boot;
    if (cfg->keyed) group_key_init(&n->group, cfg->key);
    n->last_frame = n->phase_frame = -1;
    n->rx_slot = -1;
}

void node_set_key(node_t *n, const uint8_t key[16]) {
    /* the key it has, sent again by a phone that reconnects: the riders stay */
    if (n->cfg.keyed && memcmp(n->cfg.key, key, 16) == 0) return;
    memcpy(n->cfg.key, key, 16);
    n->cfg.keyed = true;
    group_key_init(&n->group, key);
    n->phase_frame = -1;
    memset(n->riders, 0, sizeof(n->riders));
    n->echo_by = 0;
    n->echo_us = 0;
}

void node_set_phone_time(node_t *n, int64_t at_us, int64_t utc_us, int64_t err_us) {
    n->phone_time = true;
    n->phone_at_us = at_us;
    n->phone_utc_us = utc_us;
    n->phone_err_us = err_us;
}

void node_set_state(node_t *n, uint8_t state) {
    if (state >= STATES || state == n->state) return;
    n->state = state;
    n->state_sent = UINT32_MAX;
    n->echo_by = 0; /* who carried the state before has not carried this one */
}

/* ---- time ------------------------------------------------------------------------ */

static int64_t drift_ppm(const node_t *n) { return n->cfg.drift_ppm ? n->cfg.drift_ppm : DRIFT_PPM; }

bool node_utc_us(const node_t *n, int64_t now_us, int64_t *utc_us, int64_t *err_us) {
    if (n->time == TIME_NONE) return false;
    const int64_t since = now_us - n->mark_us;
    *utc_us = n->mark_utc_us + since;
    *err_us = n->mark_err_us + (since < 0 ? -since : since) * drift_ppm(n) / 1000000;
    return true;
}

bool node_utc_ms(const node_t *n, int64_t now_us, int64_t *utc_ms) {
    int64_t utc, err;
    if (!node_utc_us(n, now_us, &utc, &err)) return false;
    *utc_ms = utc / 1000;
    return true;
}

/* The calendar the riders' ages are told by: our GNSS time where we have it, else the phone's,
 * its error grown by our crystal's drift since; false with neither. */
static bool calendar_us(const node_t *n, int64_t at_us, int64_t *utc_us, int64_t *err_us, const char **source) {
    static const char *const SOURCE[] = {"none", "nmea", "group", "pps"};
    if (node_utc_us(n, at_us, utc_us, err_us)) {
        *source = SOURCE[n->time];
        return true;
    }
    *source = "none";
    if (!n->phone_time) return false;
    const int64_t since = at_us - n->phone_at_us;
    *utc_us = n->phone_utc_us + since;
    *err_us = n->phone_err_us + (since < 0 ? -since : since) * drift_ppm(n) / 1000000;
    *source = "phone";
    return true;
}

/* A new mark for our clock, taken unless the one we have, carried to now, is still better. */
static bool set_time(node_t *n, time_source_t source, int64_t at_us, int64_t utc_us, int64_t err_us,
                     int64_t now_us) {
    int64_t utc, err;
    const int64_t carried = err_us + (now_us - at_us) * drift_ppm(n) / 1000000;
    if (node_utc_us(n, now_us, &utc, &err) && err < carried) return false;
    /* The frame running when the time first comes is not ours: its slot may have gone already. */
    if (n->time == TIME_NONE) n->last_frame = utc_us / ((int64_t)n->cfg.interval_ms * 1000);
    n->time = source;
    n->mark_us = at_us;
    n->mark_utc_us = utc_us;
    n->mark_err_us = err_us;
    return true;
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
            const int64_t told = n->cfg.nmea_lag_us + (int64_t)n->cfg.nmea_lag_sat_us * fix->sats;
            set_time(n, TIME_NMEA, now_us - told, utc_us, n->cfg.nmea_err_us, now_us);
        }
    }
    if (fix->valid) {
        n->held = *fix;
        n->have_held = true;
        /* Each minute judged from where it began: 30 m on is going somewhere, a walker or a climb at
         * 7 km/h after a stop; a whole minute slow and short of that is a stop, and the next minute
         * begins there. Between 5 and 10 km/h and not stopped: rolling, no minute running. */
        const bool slow = fix->speed_kmh10 >= 0 && fix->speed_kmh10 < STOP_BELOW_KMH10;
        const bool went = n->slow_since_us &&
                          apart(fix->lat_e7, fix->lon_e7, n->slow_lat_e7, n->slow_lon_e7, STOP_WITHIN_M);
        const bool minute = n->slow_since_us && now_us - n->slow_since_us >= STOP_AFTER_US;
        if (fix->speed_kmh10 >= MOVING_OVER_KMH10 || went) n->stopped = false;
        if (slow && minute && !went) n->stopped = true;
        if (!(slow || n->stopped)) {
            n->slow_since_us = 0;
        } else if (!n->slow_since_us || went || minute) {
            n->slow_since_us = now_us;
            n->slow_lat_e7 = fix->lat_e7;
            n->slow_lon_e7 = fix->lon_e7;
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
    const int64_t gap = spacing_us(n) - phy_airtime_us(&n->phy, PKT_LEN(n->cfg.passengers)) - RX_GAP_US;
    return gap > 0 ? gap / 2 : 0;
}

bool node_time_fits(const node_t *n, int64_t now_us) {
    int64_t utc, err;
    return node_utc_us(n, now_us, &utc, &err) && err + TX_ERR_US <= node_time_budget_us(n);
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

static int64_t slot_utc_us(const node_t *n, int64_t k, int64_t phase, unsigned slot) {
    return k * frame_us(n) + phase + (int64_t)slot * spacing_us(n);
}

/* The same, with the phase kept for the frame: one AES per frame, not per poll. */
static int64_t our_slot(node_t *n, int64_t k) {
    if (k != n->phase_frame || n->phase_spacing != spacing_us(n)) {
        n->phase_us = phase_us(n, k);
        n->phase_frame = k;
        n->phase_spacing = spacing_us(n);
    }
    return slot_utc_us(n, k, n->phase_us, n->cfg.slot);
}

/* ---- sending ------------------------------------------------------------------- */

/* Whether two points lie this many metres apart or more, on a flat map around them: good to a
 * percent or two, which the send rule does not notice. */
static bool apart(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, uint32_t metres) {
    static const int16_t COS1024[10] = {1024, 1008, 962, 887, 784, 658, 512, 350, 178, 0}; /* every 10 degrees */
    const int64_t a = lat1 < 0 ? -(int64_t)lat1 : lat1;
    const int d = a >= 900000000 ? 8 : (int)(a / 100000000);
    const int64_t cos1024 = COS1024[d] + (COS1024[d + 1] - COS1024[d]) * (a - d * 100000000ll) / 100000000;
    const int64_t dy = (int64_t)lat2 - lat1, dx = ((int64_t)lon2 - lon1) * cos1024 / 1024;
    const int64_t r = (int64_t)metres * 100000 / 1113; /* 1e-7 degrees of latitude are 11.13 mm */
    if (llabs(dx) > 100000000 || llabs(dy) > 100000000) return true;
    return dx * dx + dy * dy >= r * r;
}

static uint8_t course2_of(const gnss_fix_t *h) {
    return h->course_deg10 < 0 ? COURSE_UNKNOWN : (uint8_t)(((h->course_deg10 + 10) / 20) % 180);
}

/* What of ours a change in is told at once: the state, and the facts a rider would act on. */
static uint8_t told_of(const node_t *n, bool current) {
    return (uint8_t)(n->state << 4 | (n->stopped ? FACT_STOPPED : 0) | (current ? FACT_CURRENT : 0));
}

/* The send rule (12, section 2): our slot in frame k is worth a packet. */
static bool something_to_say(const node_t *n, int64_t k, bool current) {
    if (!n->sent_any || n->burst || k - n->sent_frame >= 1ll << n->cfg.beat) return true;
    /* another rider's changed state, where we carry riders at all */
    for (int s = 0; n->cfg.passengers && s < NODE_SLOTS; s++) {
        if (n->riders[s].known && n->riders[s].news) return true;
    }
    if (!n->sent_pos) return n->have_held; /* our first position */
    if (!current) return false;
    const gnss_fix_t *h = &n->held;
    if (apart(h->lat_e7, h->lon_e7, n->sent_lat_e7, n->sent_lon_e7, n->cfg.move_m)) return true;
    const uint8_t course = course2_of(h);
    if (course == COURSE_UNKNOWN || n->sent_course2 == COURSE_UNKNOWN) return false;
    const int turn = abs((int)course - n->sent_course2);
    return (turn > 90 ? 180 - turn : turn) >= TURN_DEG2 &&
           apart(h->lat_e7, h->lon_e7, n->sent_lat_e7, n->sent_lon_e7, TURN_AFTER_M);
}

/* The riders to carry in this packet: one whose state has changed before any other, then whoever
 * we told longest ago. Latest known state passed on, not packets forwarded: a receiver keeps the
 * later record, so nothing loops and nothing floods. */
static unsigned choose_passengers(node_t *n, uint32_t sent, int64_t now_us, record_t *out) {
    unsigned count = 0;
    bool taken[NODE_SLOTS] = {false};
    while (count < n->cfg.passengers && count < PKT_PASSENGERS_MAX) {
        int best = -1;
        for (int s = 0; s < NODE_SLOTS; s++) {
            const rider_t *r = &n->riders[s], *b = best < 0 ? NULL : &n->riders[best];
            if (!r->known || taken[s] || s == n->cfg.slot || (sent > r->as_of && sent - r->as_of > RECORD_TTL_S)) continue;
            if (!b || (r->news != 0) > (b->news != 0) || ((r->news != 0) == (b->news != 0) && r->told_us < b->told_us)) best = s;
        }
        if (best < 0) break;
        rider_t *r = &n->riders[best];
        taken[best] = true;
        out[count] = r->rec;
        out[count].slot = (uint8_t)best;
        out[count].age = age_code(sent > r->as_of ? sent - r->as_of : 0);
        r->told_us = now_us ? now_us : 1;
        if (r->news) r->news--;
        count++;
    }
    return count;
}

size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap) {
    int64_t utc, err;
    /* Nothing without a time to find our slot by. */
    if (!node_utc_us(n, now_us, &utc, &err)) return 0;
    /* The radio takes tx_lead_us to start: everything below is reckoned as of the RF's beginning. */
    utc += n->cfg.tx_lead_us;
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
    /* A mode asked for that the radio has yet to take: the air would carry the old one (D15). */
    if (!phy_same(&n->phy, &n->cfg.phy)) {
        n->tx_held++;
        return 0;
    }
    /* The held position goes out with its own age; "current" says whether it is also the GNSS's
     * fix of this moment. */
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    const uint8_t told = told_of(n, current);
    if (n->sent_any && told != n->sent_told) n->burst = NEWS_TIMES;
    if (!something_to_say(n, k, current)) {
        n->tx_quiet++;
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
    /* The duty cycle, reckoned with the longest packet we may build. */
    const uint32_t limit_ms = 3600u * n->cfg.duty_permille;
    if (cap < PKT_MAX_LEN ||
        node_duty_used_ms(n, now_us) + (phy_airtime_us(&n->phy, PKT_LEN(n->cfg.passengers)) + 999) / 1000 > limit_ms) {
        n->tx_skipped++;
        return 0;
    }
    const gnss_fix_t *h = &n->held;
    const uint32_t sent = (uint32_t)(at / 1000000);
    /* The nonce must never repeat: a clock stepped back draws a new session. */
    if (n->sent_any && sent < n->last_sent) n->session = (uint16_t)next_random(n);
    const int64_t fix_s = h->utc_ms / 1000;
    packet_t p = {
        .session = n->session,
        .sent = sent,
        .count = n->count++,
        .own = {.slot = n->cfg.slot,
                .has_pos = n->have_held,
                .lat_e7 = h->lat_e7,
                .lon_e7 = h->lon_e7,
                .age = age_code(sent > fix_s ? (uint32_t)(sent - fix_s) : 0),
                .facts = (uint8_t)((n->stopped ? FACT_STOPPED : 0) | (current ? FACT_CURRENT : 0) |
                                   (n->time == TIME_PPS && err <= PRECISE_ERR_US ? FACT_TIME_PRECISE : 0)),
                .state = n->state},
        .speed_kmh = !n->have_held || h->speed_kmh10 < 0 ? SPEED_UNKNOWN
                     : h->speed_kmh10 >= 2540 ? 254 : (uint8_t)((h->speed_kmh10 + 5) / 10),
        .course2 = n->have_held ? course2_of(h) : COURSE_UNKNOWN,
        .beat = n->cfg.beat,
        .sats = current ? n->fix.sats : 0,
    };
    p.passengers = (uint8_t)choose_passengers(n, sent, now_us, p.passenger);
    const size_t len = pkt_seal(out, &p, &n->group);
    n->sent++;
    n->sent_any = true;
    n->sent_pos = n->have_held;
    n->sent_lat_e7 = h->lat_e7;
    n->sent_lon_e7 = h->lon_e7;
    n->sent_course2 = p.course2;
    n->sent_told = told;
    n->sent_frame = k;
    n->last_sent = sent;
    if (n->state_sent == UINT32_MAX) n->state_sent = sent;
    if (n->burst) n->burst--;
    n->tx_due_us = now_us - (utc - at) + n->cfg.tx_lead_us;
    n->tx_air_us = phy_airtime_us(&n->phy, len);
    duty_book(&n->duty, now_us, (n->tx_air_us + 999) / 1000);
    return len;
}

int64_t node_next_tx_us(const node_t *n, int64_t now_us) {
    int64_t utc, err;
    if (!n->cfg.keyed || n->cfg.slot >= n->cfg.slots || !node_utc_us(n, now_us, &utc, &err)) return INT64_MAX;
    utc += n->cfg.tx_lead_us;
    const int64_t k = utc / frame_us(n);
    int64_t at = slot_utc_us(n, k, phase_us(n, k), n->cfg.slot);
    if (k <= n->last_frame || utc - at > TX_LATE_US) at = slot_utc_us(n, k + 1, phase_us(n, k + 1), n->cfg.slot);
    return now_us + (at - utc);
}

static void spread_add(spread_t *s, int64_t v) {
    if (!s->n || v < s->min_us) s->min_us = v;
    if (!s->n || v > s->max_us) s->max_us = v;
    s->sum_us += v;
    s->n++;
}

/* How late it began: from its end on our UTC, less the airtime and the moment of its slot in the
 * frame nearest to it. */
void node_on_rx_done(node_t *n, int64_t done_us) {
    int64_t end_utc, end_err;
    const int slot = n->rx_slot;
    n->rx_slot = -1;
    if (slot < 0 || !node_utc_us(n, done_us, &end_utc, &end_err)) return;
    const int64_t start = end_utc - phy_airtime_us(&n->phy, n->rx_len), k = start / frame_us(n);
    int64_t late = INT64_MAX;
    for (int64_t f = k - 1; f <= k + 1; f++) {
        const int64_t off = start - slot_utc_us(n, f, phase_us(n, f), (unsigned)slot);
        if (llabs(off) < llabs(late)) late = off;
    }
    rider_t *r = &n->riders[slot];
    spread_add(&r->offset, late);
    r->late_us = late;
    /* A rider on PPS began in its slot to a fraction of a millisecond, so how late it seems is how
     * far our clock is ahead: the median of the latest few sets it, where ours is no better. The
     * packet is only ever a measure of our clock; we send by the clock, never on hearing one. */
    if (!(r->rec.facts & FACT_TIME_PRECISE) || r->via != slot || llabs(late) > SYNC_MAX_US) return;
    n->sync_us[n->sync_at] = late;
    n->sync_at = (uint8_t)((n->sync_at + 1) % NODE_SYNC);
    if (n->sync_n < NODE_SYNC) n->sync_n++;
    if (n->sync_n < SYNC_MIN) return;
    int64_t v[NODE_SYNC];
    for (int i = 0; i < n->sync_n; i++) {
        int j = i;
        for (; j > 0 && v[j - 1] > n->sync_us[i]; j--) v[j] = v[j - 1];
        v[j] = n->sync_us[i];
    }
    const int64_t ahead = v[n->sync_n / 2];
    if (!set_time(n, TIME_GROUP, done_us, end_utc - ahead, GROUP_ERR_US, done_us)) return;
    for (int i = 0; i < n->sync_n; i++) n->sync_us[i] -= ahead;
}

void node_phy_applied(node_t *n, const phy_t *phy) { n->phy = *phy; }

void node_on_tx_done(node_t *n, int64_t done_us) {
    if (!n->tx_due_us) return;
    spread_add(&n->tx_lead, done_us - n->tx_air_us - n->tx_due_us);
    n->tx_due_us = 0;
}

/* ---- receiving ------------------------------------------------------------------- */

/* A record of a rider, said by itself at as_of and told to us by via: the latest we hold now. */
static void take_record(node_t *n, const record_t *q, uint32_t as_of, uint8_t via, int64_t now_us) {
    rider_t *r = &n->riders[q->slot];
    if (r->known ? q->state != r->rec.state : q->state != STATE_NORMAL) r->news = NEWS_TIMES;
    n->rx_changed |= 1u << q->slot;
    r->known = true;
    r->rec = *q;
    r->as_of = as_of;
    r->via = via;
    r->rec_us = now_us;
}

bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us) {
    packet_t p;
    n->rx_slot = -1;
    n->rx_changed = 0;
    if (!n->cfg.keyed) {
        n->rx_foreign++;
        return false;
    }
    /* out of our reach on the bench: as if never heard, so nothing is counted */
    if (len > 2 && (n->cfg.deaf >> (buf[2] & 0x1F) & 1)) return false;
    switch (pkt_open(buf, len, &n->group, &p)) {
        case PKT_OK: break;
        case PKT_FOREIGN: n->rx_foreign++; return false;
        case PKT_FORGED: n->rx_forged++; return false;
        default: n->rx_bad++; return false;
    }
    const uint8_t from = p.own.slot;
    if (from == n->cfg.slot) {
        /* Two radios on one slot: counted, not hidden. Their sessions keep the nonces apart. */
        n->rx_clash++;
        return false;
    }
    rider_t *r = &n->riders[from];
    const uint8_t ahead = (uint8_t)(p.count - r->count);
    int64_t utc;
    /* A repeat or a replay: sent near our own time, where we have one, and each packet of a rider
     * later than the last by (sent, count). */
    if ((node_utc_ms(n, now_us, &utc) && llabs((int64_t)p.sent - utc / 1000) > SENT_SLACK_S) ||
        (r->heard && (p.sent < r->sent || (p.sent == r->sent && (ahead == 0 || ahead > 127))))) {
        n->rx_replay++;
        return false;
    }
    if (!r->heard || p.session != r->session) {
        r->received = 0;
        r->expected = 1;
    } else {
        r->expected += ahead ? ahead : 256;
    }
    r->received++;
    r->heard = true;
    r->heard_us = now_us;
    r->rssi_dbm = (int16_t)rssi_dbm;
    r->snr_db4 = (int16_t)snr_db4;
    r->session = p.session;
    r->sent = p.sent;
    r->count = p.count;
    /* What a rider says itself is the latest there is of it. Its fix's time: beyond two minutes the
     * age comes in whole minutes, rounded down, so while it stands on one held position the
     * earliest reckoning is the truest, and the fix never seems to move in time. */
    uint32_t fix = p.own.has_pos ? p.sent - age_seconds(p.own.age) : 0;
    if (fix && r->known && r->via == from && r->fix_utc && r->fix_utc < fix && !(p.own.facts & FACT_CURRENT) &&
        r->rec.lat_e7 == p.own.lat_e7 && r->rec.lon_e7 == p.own.lon_e7) {
        fix = r->fix_utc;
    }
    take_record(n, &p.own, p.sent, from, now_us);
    r->fix_utc = fix;
    r->speed_kmh = p.speed_kmh;
    r->course2 = p.course2;
    r->beat = p.beat;
    r->sats = p.sats;
    n->rx_slot = from;
    n->rx_len = len;
    for (unsigned i = 0; i < p.passengers; i++) {
        const record_t *q = &p.passenger[i];
        if (q->slot == from) continue;
        /* later than what we hold, by more than the age's own step: seconds, then minutes */
        const uint32_t ago = age_seconds(q->age), as_of = p.sent > ago ? p.sent - ago : 0;
        if (q->slot == n->cfg.slot) {
            /* ourselves, as this rider holds us: with the state we are in, and surely from a packet
             * of ours in it, it carries what we say. Surely: by the oldest the age can mean, as it
             * goes in whole minutes from two (164 s goes as 120; a review, 2026-11-22) */
            const uint32_t most = q->age < AGE_MINUTES_FROM ? ago : ago + 59;
            const bool after = q->age < AGE_OLDER && p.sent > most && p.sent - most >= n->state_sent;
            if (q->state == n->state && after) {
                n->echo_us = now_us ? now_us : 1;
                if (!(n->echo_by & 1u << from)) n->echo_news = true;
                n->echo_by |= 1u << from;
            }
            continue;
        }
        rider_t *o = &n->riders[q->slot];
        if (o->known && as_of <= o->as_of + (q->age < AGE_MINUTES_FROM ? 1 : 60)) continue;
        take_record(n, q, as_of, from, now_us);
        o->fix_utc = 0;
        o->speed_kmh = SPEED_UNKNOWN;
        o->course2 = COURSE_UNKNOWN;
        o->beat = o->sats = 0;
    }
    return true;
}

/* ---- lines ------------------------------------------------------------------------- */

/* E0's measure as text: least/mean/most in microseconds, or ? before any. */
static const char *spread_text(char *s, size_t cap, const spread_t *v) {
    if (!v->n) {
        snprintf(s, cap, "?");
    } else {
        /* ten seconds either way is as far as a measure of a slot means anything, and a status
         * line has no room for more */
        int64_t w[3] = {v->min_us, v->sum_us / (int64_t)v->n, v->max_us};
        for (int i = 0; i < 3; i++) w[i] = w[i] > 9999999 ? 9999999 : w[i] < -9999999 ? -9999999 : w[i];
        snprintf(s, cap, "%lld/%lld/%lld", (long long)w[0], (long long)w[1], (long long)w[2]);
    }
    return s;
}

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
    /* Its fix where it told us itself, else when it last said anything, which is its fix's time
     * only while its GPS was current: a held fix carried on by another has no time we know, and
     * its record's would make it young. On the calendar we have now, so a stored rider's age
     * comes, and grows truer, as our calendar does. */
    const uint32_t fix = r->fix_utc ? r->fix_utc : (r->rec.facts & FACT_CURRENT) ? r->as_of : 0;
    char lat[16], lon[16], age[24] = "?", fixs[8] = "?", via[8] = "-", direct[24] = "-";
    int64_t now_utc, err;
    const char *source;
    if (fix) snprintf(fixs, sizeof(fixs), "%u", (unsigned)(fix & 0xFFFF));
    if (fix && r->rec.has_pos && calendar_us(n, now_us, &now_utc, &err, &source)) {
        snprintf(age, sizeof(age), "%lld", (long long)(now_utc / 1000000 - fix));
    }
    char speed[8] = "?", course[8] = "?";
    if (r->speed_kmh != SPEED_UNKNOWN) snprintf(speed, sizeof(speed), "%u", r->speed_kmh);
    if (r->course2 != COURSE_UNKNOWN) snprintf(course, sizeof(course), "%u", r->course2 * 2u);
    if (r->via != slot) snprintf(via, sizeof(via), "%u", r->via);
    if (r->heard) snprintf(direct, sizeof(direct), "%lld.%lld", (long long)((now_us - r->heard_us) / 1000000), (long long)((now_us - r->heard_us) / 100000 % 10));
    const long long heard10 = (now_us - r->rec_us) / 100000;
    const int snr = r->snr_db4 < 0 ? -r->snr_db4 : r->snr_db4;
    char offset[72];
    /* heard= is since anything came of it, itself or told; direct= since itself. answered= is v3's,
     * kept empty for the app until it speaks this version. */
    return snprintf(buf, cap,
                    "RIDER slot=%u group=%08lx boot=%08lx at=%lld heard_at=%lld fix=%s pos=%d lat=%s lon=%s fix_age=%s heard=%lld.%lld speed=%s course=%s gps=%d "
                    "help=%d stopped=%d answered=- state=%u via=%s direct=%s rssi=%d snr=%s%d.%02d rx=%lu/%lu offset=%s late=%ld sats=%u",
                    slot, (unsigned long)n->group.id, (unsigned long)n->boot, (long long)(now_us / 1000),
                    (long long)(r->rec_us / 1000), fixs, r->rec.has_pos ? 1 : 0,
                    deg(lat, sizeof(lat), r->rec.lat_e7), deg(lon, sizeof(lon), r->rec.lon_e7),
                    age, heard10 / 10, heard10 % 10, speed, course,
                    (r->rec.facts & FACT_CURRENT) ? 1 : 0, r->rec.state == STATE_HELP ? 1 : 0,
                    (r->rec.facts & FACT_STOPPED) ? 1 : 0, r->rec.state, via, direct, r->rssi_dbm, r->snr_db4 < 0 ? "-" : "",
                    snr / 4, snr % 4 * 25, (unsigned long)r->received, (unsigned long)r->expected,
                    spread_text(offset, sizeof(offset), &r->offset),
                    /* the packet this line is told for, where its RxDone was stamped: a bench reads
                     * each one here, with no reckoning back from the mean */
                    (long)(r->late_us > 9999999 ? 9999999 : r->late_us < -9999999 ? -9999999 : r->late_us), r->sats);
}

int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap) {
    const uint32_t used = node_duty_used_ms(n, now_us);
    int known = 0;
    for (int i = 0; i < NODE_SLOTS; i++) known += n->riders[i].known;
    /* heard= riders who carry the state we are in; echo= seconds since one last did, 9999 at most;
     * send= the send rule's heartbeat, metres and passengers. Our own position is not here: a line
     * holds 511 characters, and nothing read it. */
    char err_ms[48] = "?", lag[48] = "?", txlead[72], echo[24] = "-";
    const int64_t echo_s = (now_us - n->echo_us) / 1000000;
    if (n->echo_us) snprintf(echo, sizeof(echo), "%d", echo_s > 9999 ? 9999 : (int)echo_s);
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    static const char *const SOURCE[] = {"none", "nmea", "group", "pps"};
    int64_t utc, err;
    if (node_utc_us(n, now_us, &utc, &err)) {
        if (err > 99999999) err = 99999999; /* 100 s: past any budget, and the line has no room for more */
        snprintf(err_ms, sizeof(err_ms), "%lld.%lld", (long long)(err / 1000), (long long)(err % 1000 / 100));
    }
    /* the calendar the riders' ages are told by, and how far off it may be, in ms */
    char cal[48];
    const char *source;
    if (calendar_us(n, now_us, &utc, &err, &source)) {
        snprintf(cal, sizeof(cal), "%s/%lld", source, (long long)((err > 99999999 ? 99999999 : err) / 1000));
    } else {
        snprintf(cal, sizeof(cal), "none");
    }
    if (n->lag_us) {
        /* under PPS_PAIR_US, all three */
        snprintf(lag, sizeof(lag), "%d/%d/%d", (int)(n->lag_us / 1000), (int)(n->lag_min_us / 1000),
                 (int)(n->lag_max_us / 1000));
    }
    const int64_t budget = node_time_budget_us(n);
    return snprintf(buf, cap,
                    "slot=%u slots=%u keyed=%d group=%08lx mode=%u/%lu fix=%d stopped=%d heard=%d state=%u echo=%s send=%u/%u/%u deaf=%lx sats=%u "
                    "time=%s err=%s cal=%s "
                    "budget=%lld.%lld lag=%s txlead=%s tx=%lu late=%lu unsure=%lu skipped=%lu quiet=%lu held=%lu duty=%lu.%02lu%% riders=%d "
                    "bad=%lu foreign=%lu forged=%lu replay=%lu clash=%lu",
                    n->cfg.slot, n->cfg.slots, n->cfg.keyed ? 1 : 0, (unsigned long)(n->cfg.keyed ? n->group.id : 0), n->cfg.phy.sf,
                    (unsigned long)(n->cfg.phy.bw_hz / 1000),
                    current ? 1 : 0, n->stopped ? 1 : 0, __builtin_popcount(n->echo_by), n->state, echo,
                    n->cfg.beat, n->cfg.move_m, n->cfg.passengers, (unsigned long)n->cfg.deaf,
                    n->fix.sats, SOURCE[n->time], err_ms, cal, (long long)(budget / 1000),
                    (long long)(budget % 1000 / 100), lag, spread_text(txlead, sizeof(txlead), &n->tx_lead),
                    (unsigned long)n->sent, (unsigned long)n->tx_late,
                    (unsigned long)n->tx_unsure, (unsigned long)n->tx_skipped, (unsigned long)n->tx_quiet, (unsigned long)n->tx_held,
                    (unsigned long)(used / 36000),
                    (unsigned long)(used % 36000 / 360), known, (unsigned long)n->rx_bad,
                    (unsigned long)n->rx_foreign, (unsigned long)n->rx_forged, (unsigned long)n->rx_replay,
                    (unsigned long)n->rx_clash);
}
