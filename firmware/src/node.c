#include "node.h"

#include <stdio.h>
#include <string.h>

/* How old our own fix may be and still count as the GNSS's current one. */
#define FIX_FRESH_US (3 * 1000000ll)

static uint32_t next_random(node_t *n) {
    /* xorshift32: enough to keep two senders from falling into step */
    uint32_t x = n->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return n->rng = x;
}

/* The interval, drawn anew each time between 0.8 and 1.2 of it: two senders that start in step
 * do not stay there (dmd2-lora-bridge's 2026-10-01 ride). */
static int64_t jittered_us(node_t *n) {
    const uint32_t ms = n->cfg.interval_ms;
    return (int64_t)(ms * 4 / 5 + next_random(n) % (ms * 2 / 5 + 1)) * 1000;
}

void node_init(node_t *n, const node_config_t *cfg, uint32_t seed, int64_t now_us) {
    memset(n, 0, sizeof(*n));
    n->cfg = *cfg;
    n->rng = seed ? seed : 0x9E3779B9u;
    /* The first packet anywhere in one interval, so boards switched on together spread out. */
    n->next_tx_us = now_us + (int64_t)(next_random(n) % (cfg->interval_ms + 1)) * 1000;
}

void node_on_fix(node_t *n, const gnss_fix_t *fix, int64_t now_us) {
    n->fix = *fix;
    n->fix_us = now_us;
    if (fix->time_valid) {
        n->have_time = true;
        n->time_utc_ms = fix->utc_ms;
        n->time_us = now_us;
    }
    if (fix->valid) {
        n->held = *fix;
        n->have_held = true;
    }
}

bool node_utc_ms(const node_t *n, int64_t now_us, int64_t *utc_ms) {
    if (!n->have_time) return false;
    *utc_ms = n->time_utc_ms + (now_us - n->time_us) / 1000;
    return true;
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

/* ---- sending ------------------------------------------------------------------- */

size_t node_poll_tx(node_t *n, int64_t now_us, uint8_t *out, size_t cap) {
    if (now_us < n->next_tx_us) return 0;
    n->next_tx_us = now_us + jittered_us(n);
    /* Nothing true to say before the first fix: no position, no time to stamp it with. */
    const uint32_t air_ms = (phy_airtime_us(&n->cfg.phy, PKT_POSITION_LEN) + 999) / 1000;
    const uint32_t limit_ms = 3600u * n->cfg.duty_permille;
    if (!n->have_held || !n->have_time || cap < PKT_POSITION_LEN ||
        node_duty_used_ms(n, now_us) + air_ms > limit_ms) {
        n->tx_skipped++;
        return 0;
    }
    const gnss_fix_t *h = &n->held;
    /* The held position goes out with its own time, so it ages honestly; GPS_OK says whether it
     * is also the GNSS's current fix. */
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    position_t p = {
        .group = n->cfg.group,
        .slot = n->cfg.slot,
        .counter = n->counter++ & 0xFFFFFFu,
        .fix_time = (uint16_t)((h->utc_ms / 1000) & 0xFFFF),
        .lat_e7 = h->lat_e7,
        .lon_e7 = h->lon_e7,
        .speed_kmh = h->speed_kmh10 < 0 ? SPEED_UNKNOWN
                     : h->speed_kmh10 >= 2540 ? 254 : (uint8_t)((h->speed_kmh10 + 5) / 10),
        .course2 = h->course_deg10 < 0 ? COURSE_UNKNOWN : (uint8_t)(((h->course_deg10 + 10) / 20) % 180),
        .status = current ? ST_GPS_OK : 0,
    };
    duty_book(&n->duty, now_us, air_ms);
    return pkt_encode(out, &p);
}

/* ---- receiving ------------------------------------------------------------------- */

bool node_on_packet(node_t *n, const uint8_t *buf, size_t len, int rssi_dbm, int snr_db4, int64_t now_us) {
    position_t p;
    if (!pkt_decode(buf, len, &p) || p.slot >= NODE_SLOTS) {
        n->rx_bad++;
        return false;
    }
    if (p.group != n->cfg.group) {
        n->rx_foreign++;
        return false;
    }
    if (p.slot == n->cfg.slot) {
        /* Two radios on one slot: in v1 their nonces would repeat, so it is counted, not hidden. */
        n->rx_clash++;
        return false;
    }
    rider_t *r = &n->riders[p.slot];
    if (r->known && p.counter == r->last.counter) return false; /* the same packet again */
    if (!r->known || p.counter < r->last.counter) {
        /* new, or its counter went back: it restarted, and its run of counters starts here */
        r->first_counter = p.counter;
        r->received = 0;
    }
    r->received++;
    r->known = true;
    r->last = p;
    r->heard_us = now_us;
    r->rssi_dbm = (int16_t)rssi_dbm;
    r->snr_db4 = (int16_t)snr_db4;
    int64_t utc;
    if (node_utc_ms(n, now_us, &utc)) {
        /* The fix time is UTC seconds modulo 65536: unwrapped to the nearest, past or future. A
         * future one is a clock gone wrong, kept as such rather than read as fresh. */
        const int64_t now_s = utc / 1000;
        const uint16_t behind = (uint16_t)((uint16_t)now_s - p.fix_time);
        r->fix_utc_ms = (behind < 0x8000 ? now_s - behind : now_s + (0x10000 - behind)) * 1000;
    } else {
        r->fix_utc_ms = 0;
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
    char speed[8] = "?", course[8] = "?";
    if (r->last.speed_kmh != SPEED_UNKNOWN) snprintf(speed, sizeof(speed), "%u", r->last.speed_kmh);
    if (r->last.course2 != COURSE_UNKNOWN) snprintf(course, sizeof(course), "%u", r->last.course2 * 2u);
    const long long heard10 = (now_us - r->heard_us) / 100000;
    const int snr = r->snr_db4 < 0 ? -r->snr_db4 : r->snr_db4;
    return snprintf(buf, cap,
                    "RIDER slot=%u lat=%s lon=%s fix_age=%s heard=%lld.%lld speed=%s course=%s gps=%d "
                    "help=%d stopped=%d rssi=%d snr=%s%d.%02d rx=%lu/%lu",
                    slot, deg(lat, sizeof(lat), r->last.lat_e7), deg(lon, sizeof(lon), r->last.lon_e7),
                    age, heard10 / 10, heard10 % 10, speed, course,
                    (r->last.status & ST_GPS_OK) ? 1 : 0, (r->last.status & ST_HELP) ? 1 : 0,
                    (r->last.status & ST_STOPPED) ? 1 : 0, r->rssi_dbm, r->snr_db4 < 0 ? "-" : "",
                    snr / 4, snr % 4 * 25, (unsigned long)r->received,
                    (unsigned long)(r->last.counter - r->first_counter + 1));
}

int node_format_status(const node_t *n, int64_t now_us, char *buf, size_t cap) {
    const uint32_t used = node_duty_used_ms(n, now_us);
    int known = 0;
    for (int i = 0; i < NODE_SLOTS; i++) known += n->riders[i].known;
    char lat[16], lon[16];
    const bool current = n->fix.valid && now_us - n->fix_us <= FIX_FRESH_US;
    return snprintf(buf, cap,
                    "slot=%u group=%u fix=%d lat=%s lon=%s sats=%u hdop=%u.%u time=%d tx=%lu "
                    "skipped=%lu duty=%lu.%02lu%% riders=%d bad=%lu foreign=%lu clash=%lu",
                    n->cfg.slot, n->cfg.group, current ? 1 : 0, deg(lat, sizeof(lat), n->held.lat_e7),
                    deg(lon, sizeof(lon), n->held.lon_e7), n->fix.sats, n->fix.hdop10 / 10,
                    n->fix.hdop10 % 10, n->have_time ? 1 : 0, (unsigned long)n->counter,
                    (unsigned long)n->tx_skipped, (unsigned long)(used / 36000),
                    (unsigned long)(used % 36000 / 360), known, (unsigned long)n->rx_bad,
                    (unsigned long)n->rx_foreign, (unsigned long)n->rx_clash);
}
