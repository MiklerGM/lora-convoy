/*
 * Several node cores on simulated air (ARCHITECTURE, testing without hardware).
 *
 * The air: everyone hears everyone, or, given a range, everyone within it and fewer and fewer up to
 * half as far again. A packet is lost to a receiver that transmits during any of it, to one that
 * hears two transmissions overlap (no capture effect), and at random with a set probability. Each
 * rider rides a circle around Vaduz, or a road out of it in a column that spreads out. Each board has its own clock, off by its
 * crystal's drift, and a GNSS that gives a PPS edge on the second, where the board has the line,
 * and its fix some 100 ms later; for some, nothing for a while, as in a tunnel.
 *
 *   sim                       the checks: slots keep a group clear of itself, whatever the clocks
 *   sim N INTERVAL_MS S       N riders, every INTERVAL_MS, for S seconds of virtual time
 *   sim serve N [PORT]        N boards in real time, each with the phone API on 127.0.0.1:PORT+i
 *                             (default 4403); an Android emulator reaches them at 10.0.2.2. Like
 *                             boards fresh from flashing, they have no key: KEY sets one
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "api.h"
#include "node.h"

#define PI 3.14159265358979323846

#define SEC 1000000ll
#define STEP_US 1000
#define MAX_NODES NODE_SLOTS
#define MAX_AIR 256
/* Kept this long after it ends, longer than any packet lasts, so later ones still see it overlap. */
#define KEEP_US (2 * SEC)

/* Records of other riders each packet carries here: -DSIM_PASSENGERS=0..3 to compare. */
#ifndef SIM_PASSENGERS
#define SIM_PASSENGERS 1
#endif

/* A rider's fix in UTC ms as a node holds it, or when it last said anything where a passenger told of it; 0 before one. */
static int64_t fix_ms(const rider_t *r) { return (int64_t)(r->fix_utc ? r->fix_utc : r->as_of) * 1000; }

/* v3's call for help, which these scenarios were written with: states 1 and 0. */
static void node_set_help(node_t *n, bool on) { node_set_state(n, on ? STATE_HELP : STATE_NORMAL); }

typedef struct {
    int from;
    int64_t start, end;
    bool done;
    uint8_t bytes[PKT_MAX_LEN];
    size_t len;
} transmission_t;

/* Each board's clock and GNSS. */
typedef struct {
    double drift_ppm;      /* each board's crystal, drawn within +- this of true time */
    int nmea_only;         /* this many boards, the first ones, have no PPS line, as the L1 */
    int64_t lag_us;        /* each GNSS's NMEA fix comes this long after its second ... */
    int64_t jitter_us;     /* ... give or take this, drawn for every fix */
    int64_t told_lag_us;   /* what the boards are told of that lag (cfg.nmea_lag_us) ... */
    int64_t told_err_us;   /* ... and how far it may be off (cfg.nmea_err_us) */
    int dark;              /* this many boards, the last ones, lose their GNSS ... */
    int64_t dark_from_us;  /* ... from here ... */
    int64_t dark_us;       /* ... for this long: a tunnel */
} clocks_t;

/* PPS on every board, perfect crystals, the fix 100 ms after its second. */
static const clocks_t GOOD = {.lag_us = 100000, .told_err_us = 100000};

typedef struct {
    int nodes;
    uint32_t interval_ms;
    int64_t duration_us;
    double loss;           /* random loss per packet and receiver */
    uint32_t seed;
    int groups;            /* riders split into this many groups, each with its own key; 0: no key */
    clocks_t clocks;
    double range_m;        /* how far a radio is heard for sure; 0 for everywhere */
    bool column;           /* riders on one road at their own speeds, one stopping, not on circles */
} scenario_t;

typedef struct {
    double delivered;        /* of all packets sent, the share the others of its group took */
    double worst;            /* the same for the rider worst off, of those who sent */
    double age_p50, age_p95; /* seconds: the age of the fix each node shows for each other of its
                                group, sampled every second (07's headline numbers); never seen
                                counts as forever */
    int collisions;          /* packets that overlapped another ... */
    int within;              /* ... from their own group */
    double gap;              /* seconds: the longest a rider went without a packet from another of
                                its group, the worst pair, its last gap up to the end included */
    double gap_group;        /* seconds: the longest a rider went hearing none of its group at all */
    int sent;
    int silent;              /* riders who sent nothing */
    unsigned long unsure;    /* slots passed over for an uncertain time, all riders together */
} outcome_t;

/* The simulated world: the nodes, the air between them, and what happened. */
typedef struct {
    scenario_t sc;
    node_t node[MAX_NODES];
    transmission_t air[MAX_AIR];
    int in_air;
    uint32_t rng;
    int64_t utc0_ms;
    struct {
        double drift;        /* the crystal's error, a fraction */
        int64_t offset_us;   /* its clock at true zero: boards start apart */
        bool fix_pending;    /* a fix on its way over the UART ... */
        int64_t fix_due;     /* ... arriving then, true time ... */
        int64_t fix_second;  /* ... and naming this second */
    } clock[MAX_NODES];
    int group_of[MAX_NODES], slot_of[MAX_NODES], size_of[MAX_NODES]; /* size: of i's group */
    double north_m[MAX_NODES], east_m[MAX_NODES]; /* where each rider is, from Vaduz, this second */
    int sent, collisions, within, sent_by[MAX_NODES];
    long taken, expected, taken_from[MAX_NODES];
    /* Gaps, in true time from the warm-up on: when each node last took a packet from each other,
     * and from anyone of its group, and the longest it went without (09). */
    int64_t heard_at[MAX_NODES][MAX_NODES], gap[MAX_NODES][MAX_NODES];
    int64_t any_at[MAX_NODES], any_gap[MAX_NODES];
    int64_t gaps_from;
    /* told of every packet a node takes, for a phone connected to it */
    void (*on_taken)(void *ctx, int node, unsigned slot, int64_t t);
    void *ctx;
    /* The comparison runner controls only the environment and observes real node-core events. */
    bool (*link_at)(void *ctx, int from, int to, int64_t t);
    bool (*packet_lost)(void *ctx, const transmission_t *x, int receiver);
    bool (*external_busy)(void *ctx, const transmission_t *x);
    unsigned (*gnss_mask)(void *ctx, int node, int64_t t); /* bit 0: no GNSS; bit 1: no position */
    void (*on_tx)(void *ctx, const transmission_t *x);
    void (*on_rx)(void *ctx, const transmission_t *x, int receiver, bool accepted, const char *reason);
} world_t;

/* Independent seeds per node. Successive outputs of one xorshift would give each node the same
 * sequence shifted by a step, which locks their jitter together and hides collisions. */
static uint32_t splitmix(uint32_t x) {
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

static int by_value(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static uint32_t rnd(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* A rider on a circle of 300-900 m around Vaduz, at 30-60 km/h. */
static gnss_fix_t ride(int i, int64_t t_us, int64_t utc0_ms) {
    const double r = 300 + 100 * (i % 7), kmh = 30 + 5 * (i % 7), phase = i * 0.7;
    const double theta = phase + kmh / 3.6 / r * (double)t_us / SEC;
    const double lat = 47.1410 + r * cos(theta) / 111320.0;
    const double lon = 9.5215 + r * sin(theta) / (111320.0 * cos(47.1410 * PI / 180));
    double course = atan2(cos(theta), -sin(theta)) * 180 / PI;
    if (course < 0) course += 360;
    return (gnss_fix_t){.time_valid = true, .valid = true, .utc_ms = utc0_ms + t_us / 1000,
                        .lat_e7 = (int32_t)lround(lat * 1e7), .lon_e7 = (int32_t)lround(lon * 1e7),
                        .speed_kmh10 = (int32_t)(kmh * 10), .course_deg10 = (int32_t)(course * 10),
                        .sats = 9, .hdop10 = 9};
}

/* Board i's clock at true time t. */
static int64_t local_us(const world_t *w, int i, int64_t t) {
    return w->clock[i].offset_us + t + (int64_t)((double)t * w->clock[i].drift);
}

/* A column on a road north-east out of Vaduz: rider i starts 100 m behind the one before, rides at
 * 50-74 km/h, and the column spreads out; rider STOPPER stops for five minutes. */
#define STOPPER 5
#define STOP_FROM (600 * SEC)
#define STOP_FOR (300 * SEC)

static double column_m(int i, int64_t t_us) {
    const double kmh = 50 + (i * 7) % 25;
    int64_t moving = t_us;
    if (i == STOPPER && t_us > STOP_FROM) moving -= t_us - STOP_FROM < STOP_FOR ? t_us - STOP_FROM : STOP_FOR;
    return -100.0 * i + kmh / 3.6 * (double)moving / SEC;
}

static gnss_fix_t column_fix(int i, int64_t t_us, int64_t utc0_ms) {
    const double m = column_m(i, t_us), north = m * cos(PI / 4), east = m * sin(PI / 4);
    const bool stopped = i == STOPPER && t_us > STOP_FROM && t_us < STOP_FROM + STOP_FOR;
    return (gnss_fix_t){.time_valid = true, .valid = true, .utc_ms = utc0_ms + t_us / 1000,
                        .lat_e7 = (int32_t)lround((47.1410 + north / 111320.0) * 1e7),
                        .lon_e7 = (int32_t)lround((9.5215 + east / (111320.0 * cos(47.1410 * PI / 180))) * 1e7),
                        .speed_kmh10 = stopped ? 0 : (50 + (i * 7) % 25) * 10, .course_deg10 = 450,
                        .sats = 9, .hdop10 = 9};
}

static gnss_fix_t where(const world_t *w, int i, int64_t t_us) {
    return w->sc.column ? column_fix(i, t_us, w->utc0_ms) : ride(i, t_us, w->utc0_ms);
}

static double distance_m(const world_t *w, int a, int b) {
    const double dn = w->north_m[a] - w->north_m[b], de = w->east_m[a] - w->east_m[b];
    return sqrt(dn * dn + de * de);
}

/* Whether receiver i is in reach of sender j, this once: for sure within the range, then less and
 * less likely up to half as far again. */
static bool reaches(world_t *w, int j, int i) {
    if (w->sc.range_m <= 0) return true;
    const double d = distance_m(w, i, j), r = w->sc.range_m;
    if (d <= r) return true;
    if (d >= 1.5 * r) return false;
    return (double)(rnd(&w->rng) % 1000000) / 1e6 < (1.5 * r - d) / (0.5 * r);
}

/* The gap ending at t, since *at or the warm-up's end, kept if the longest yet. */
static void gap_to(const int64_t *at, int64_t *longest, int64_t t, int64_t from) {
    const int64_t since = *at > from ? *at : from;
    if (t - since > *longest) *longest = t - since;
}

static void world_init(world_t *w, const scenario_t *sc, int64_t utc0_ms) {
    memset(w, 0, sizeof(*w));
    w->sc = *sc;
    w->gaps_from = 10 * (int64_t)sc->interval_ms * 1000;
    w->rng = sc->seed ? sc->seed : 1;
    w->utc0_ms = utc0_ms;
    /* Riders split into groups by halves, thirds ...; group g has the key g+1, g+1 ..., and its
     * riders the slots from 0. */
    for (int i = 0; i < sc->nodes; i++) {
        w->group_of[i] = sc->groups ? i * sc->groups / sc->nodes : 0;
        w->slot_of[i] = i && w->group_of[i - 1] == w->group_of[i] ? w->slot_of[i - 1] + 1 : 0;
    }
    for (int i = 0; i < sc->nodes; i++) {
        for (int j = 0; j < sc->nodes; j++) w->size_of[i] += w->group_of[j] == w->group_of[i];
    }
    for (int i = 0; i < sc->nodes; i++) {
        const clocks_t *c = &sc->clocks;
        node_config_t cfg = {.keyed = sc->groups > 0, .slot = (uint8_t)w->slot_of[i],
                             .slots = (uint8_t)w->size_of[i], .interval_ms = sc->interval_ms,
                             .duty_permille = 100, .phy = PHY_V0, .nmea_lag_us = (int32_t)c->told_lag_us,
                             .nmea_err_us = (int32_t)c->told_err_us, .listen_one_in = 32, .passengers = SIM_PASSENGERS};
        memset(cfg.key, w->group_of[i] + 1, sizeof(cfg.key));
        node_init(&w->node[i], &cfg, splitmix(sc->seed * 1000u + (uint32_t)i));
        w->clock[i].drift = c->drift_ppm * 1e-6 * ((double)(rnd(&w->rng) % 2001) / 1000 - 1);
        w->clock[i].offset_us = rnd(&w->rng) % (10 * SEC);
    }
}

static void world_step(world_t *w, int64_t t) {
    const clocks_t *c = &w->sc.clocks;
    for (int i = 0; i < w->sc.nodes; i++) {
        /* the GNSS: on the second a PPS edge, where the board has the line, and later the fix */
        const bool dark = (w->gnss_mask && (w->gnss_mask(w->ctx, i, t) & 1)) ||
                          (i >= w->sc.nodes - c->dark && t >= c->dark_from_us && t < c->dark_from_us + c->dark_us);
        if (t % SEC == 0 && !dark) {
            if (i >= c->nmea_only) node_on_pps(&w->node[i], local_us(w, i, t));
            const int64_t jitter =
                c->jitter_us ? (int64_t)(rnd(&w->rng) % (uint32_t)(2 * c->jitter_us + 1)) - c->jitter_us : 0;
            w->clock[i].fix_pending = true;
            w->clock[i].fix_due = t + c->lag_us + jitter;
            w->clock[i].fix_second = t;
        }
        if (t % SEC == 0) {
            const gnss_fix_t here = where(w, i, t);
            w->north_m[i] = (here.lat_e7 / 1e7 - 47.1410) * 111320.0;
            w->east_m[i] = (here.lon_e7 / 1e7 - 9.5215) * 111320.0 * cos(47.1410 * PI / 180);
        }
        if (w->clock[i].fix_pending && t >= w->clock[i].fix_due) {
            gnss_fix_t f = where(w, i, w->clock[i].fix_second);
            if (w->gnss_mask && (w->gnss_mask(w->ctx, i, w->clock[i].fix_second) & 2)) f.valid = false;
            node_on_fix(&w->node[i], &f, local_us(w, i, w->clock[i].fix_due));
            w->clock[i].fix_pending = false;
        }
        uint8_t b[PKT_MAX_LEN];
        const size_t len = node_poll_tx(&w->node[i], local_us(w, i, t), b, sizeof(b));
        if (len) {
            assert(w->in_air < MAX_AIR);
            transmission_t *x = &w->air[w->in_air++];
            x->from = i;
            x->start = t;
            x->end = t + phy_airtime_us(&w->node[i].phy, len);   /* in the mode its radio has */
            x->done = false;
            memcpy(x->bytes, b, len);
            x->len = len;
            w->sent++;
            w->sent_by[i]++;
            w->expected += w->size_of[i] - 1;
            if (w->on_tx) w->on_tx(w->ctx, x);
        }
    }
    /* deliver what ended now */
    for (int k = 0; k < w->in_air; k++) {
        transmission_t *x = &w->air[k];
        if (x->done || x->end > t) continue;
        x->done = true;
        node_on_tx_done(&w->node[x->from], local_us(w, x->from, x->end));
        bool collided = false, within = false;
        for (int j = 0; j < w->in_air; j++) {
            if (j != k && w->air[j].start < x->end && w->air[j].end > x->start) {
                collided = true;
                within |= w->group_of[w->air[j].from] == w->group_of[x->from];
            }
        }
        w->collisions += collided;
        w->within += within;
        for (int i = 0; i < w->sc.nodes; i++) {
            if (i == x->from) continue;
            const bool reachable = w->link_at ? (w->link_at(w->ctx, x->from, i, x->start) &&
                                                 w->link_at(w->ctx, x->from, i, x->end - 1))
                                             : reaches(w, x->from, i);
            if (!reachable) {
                if (w->on_rx) w->on_rx(w->ctx, x, i, false, "unreachable");
                continue;
            }
            /* half duplex: a receiver sending during any of it hears none of it; and one that also
             * hears another packet over it hears neither */
            bool lost = false;
            for (int j = 0; j < w->in_air && !lost; j++) {
                const transmission_t *o = &w->air[j];
                if (j == k || o->start >= x->end || o->end <= x->start) continue;
                const int64_t overlap = o->start > x->start ? o->start : x->start;
                const int64_t overlap_end = (o->end < x->end ? o->end : x->end) - 1;
                const bool other_heard = w->link_at ? (w->link_at(w->ctx, o->from, i, overlap) ||
                                                       w->link_at(w->ctx, o->from, i, overlap_end))
                                                   : (w->sc.range_m <= 0 ||
                                                      distance_m(w, o->from, i) < 1.5 * w->sc.range_m);
                lost = o->from == i || other_heard;
            }
            if (lost) {
                if (w->on_rx) w->on_rx(w->ctx, x, i, false, "interference");
                continue;
            }
            if (w->external_busy && w->external_busy(w->ctx, x)) {
                if (w->on_rx) w->on_rx(w->ctx, x, i, false, "external_interference");
                continue;
            }
            const bool random_loss = w->packet_lost ? w->packet_lost(w->ctx, x, i)
                                                    : (double)(rnd(&w->rng) % 1000000) / 1e6 < w->sc.loss;
            if (random_loss) {
                if (w->on_rx) w->on_rx(w->ctx, x, i, false, "loss");
                continue;
            }
            /* at its end, as a radio's RxDone comes, not at the step that found it ended */
            const bool accepted = node_on_packet(&w->node[i], x->bytes, x->len, -90, 20, local_us(w, i, x->end));
            if (accepted) {
                node_on_rx_done(&w->node[i], local_us(w, i, x->end));
                w->taken++;
                w->taken_from[x->from]++;
                gap_to(&w->heard_at[i][x->from], &w->gap[i][x->from], t, w->gaps_from);
                gap_to(&w->any_at[i], &w->any_gap[i], t, w->gaps_from);
                w->heard_at[i][x->from] = w->any_at[i] = t;
                if (w->on_taken) w->on_taken(w->ctx, i, x->bytes[2], local_us(w, i, t));
            }
            if (w->on_rx) w->on_rx(w->ctx, x, i, accepted, accepted ? "accepted" : "rejected");
        }
    }
    for (int k = 0; k < w->in_air;) {
        if (w->air[k].done && w->air[k].end + KEEP_US < t) {
            w->air[k] = w->air[--w->in_air];
        } else {
            k++;
        }
    }
}

/* The last run's world, for a check to look into. */
static world_t s_run;

static outcome_t run(const scenario_t *sc, bool print) {
    world_init(&s_run, sc, 1792670400000ll);
    outcome_t out = {0};
    const size_t max_ages = (size_t)(sc->duration_us / SEC + 1) * (size_t)(sc->nodes * sc->nodes);
    double *ages = malloc(max_ages * sizeof(double));
    size_t n_ages = 0;
    assert(ages);
    for (int64_t t = 0; t < sc->duration_us; t += STEP_US) {
        world_step(&s_run, t);
        /* what each map shows of its group, once a second, after a warm-up of ten intervals */
        if (t % SEC == SEC / 2 && t > 10 * (int64_t)sc->interval_ms * 1000) {
            for (int i = 0; i < sc->nodes; i++) {
                int64_t utc;
                if (!node_utc_ms(&s_run.node[i], local_us(&s_run, i, t), &utc)) continue;
                for (int j = 0; j < sc->nodes; j++) {
                    if (j == i || s_run.group_of[j] != s_run.group_of[i] || n_ages >= max_ages) continue;
                    const rider_t *r = &s_run.node[i].riders[s_run.slot_of[j]];
                    ages[n_ages++] = r->known ? (double)(utc - fix_ms(r)) / 1000 : 1e9;
                }
            }
        }
    }
    /* The end of the run: what one node shows for the next. */
    char line[API_LINE];
    for (int i = 0; print && i < sc->nodes; i++) {
        node_format_rider(&s_run.node[i], (unsigned)((s_run.slot_of[i] + 1) % s_run.size_of[i]),
                          local_us(&s_run, i, sc->duration_us), line, sizeof(line));
        printf("node %2d sees %s\n", i, line);
    }
    qsort(ages, n_ages, sizeof(double), by_value);
    out.age_p50 = n_ages ? ages[n_ages / 2] : 1e9;
    out.age_p95 = n_ages ? ages[n_ages * 95 / 100] : 1e9;
    free(ages);
    for (int i = 0; i < sc->nodes; i++) {
        gap_to(&s_run.any_at[i], &s_run.any_gap[i], sc->duration_us, s_run.gaps_from);
        out.gap_group = fmax(out.gap_group, (double)s_run.any_gap[i] / SEC);
        for (int j = 0; j < sc->nodes; j++) {
            if (j == i || s_run.group_of[j] != s_run.group_of[i]) continue;
            gap_to(&s_run.heard_at[i][j], &s_run.gap[i][j], sc->duration_us, s_run.gaps_from);
            out.gap = fmax(out.gap, (double)s_run.gap[i][j] / SEC);
        }
    }
    out.sent = s_run.sent;
    out.collisions = s_run.collisions;
    out.within = s_run.within;
    out.delivered = s_run.expected ? (double)s_run.taken / (double)s_run.expected : 0;
    out.worst = 1;
    for (int i = 0; i < sc->nodes; i++) {
        out.unsure += s_run.node[i].tx_unsure;
        if (!s_run.sent_by[i]) {
            out.silent++;
            continue;
        }
        const double share = (double)s_run.taken_from[i] / ((double)s_run.sent_by[i] * (s_run.size_of[i] - 1));
        if (share < out.worst) out.worst = share;
    }
    return out;
}

/* ---- reproducible comparisons, using the same node core ------------------------------ */

#define COMPARE_EVENTS 128

typedef struct {
    int64_t at_us;
    int source;
    bool help, delivered[MAX_NODES];
} compare_state_t;

typedef struct {
    int64_t from_us, until_us;
    int from, to;
} compare_cut_t;

typedef struct {
    int64_t from_us, until_us;
    int node;                     /* -1: all nodes */
    unsigned mask;
} compare_outage_t;

typedef struct {
    char name[48];
    clocks_t clocks;
    int loss_permille;
    int64_t interference_period_us, interference_busy_us, interference_phase_us;
    compare_outage_t outages[COMPARE_EVENTS];
    int n_outages;
} compare_environment_t;

typedef struct {
    char name[48];
    int nodes, interval_ms, duration_s, warmup_s, loss_permille;
    bool links[MAX_NODES][MAX_NODES];
    compare_state_t states[COMPARE_EVENTS];
    compare_cut_t cuts[COMPARE_EVENTS];
    int n_states, n_cuts;
} compare_config_t;

typedef struct {
    compare_config_t cfg;
    compare_environment_t environment;
    world_t *world;
    uint32_t seed;
    double *ages;
    size_t n_ages, age_capacity;
    uint64_t samples, unseen, clock_unknown, path_unavailable;
    int64_t last_rx[MAX_NODES][MAX_NODES], longest_gap_us;
    int64_t last_update[MAX_NODES][MAX_NODES], last_fix[MAX_NODES][MAX_NODES];
    uint64_t air_us[MAX_NODES];
    double state_delays[COMPARE_EVENTS * MAX_NODES];
    int n_state_delays, rx_accepted, contract_failures;
    uint64_t rx_attempts, rx_unreachable, rx_interference, rx_external_interference, rx_loss, rx_rejected;
    int64_t clock_error_max_us;
    uint64_t clock_bound_exceeded;
} comparison_t;

static bool compare_integer(const char *s, int64_t low, int64_t high, int64_t *value) {
    char *end;
    errno = 0;
    const long long n = strtoll(s, &end, 10);
    if (!s[0] || *end || errno || n < low || n > high) return false;
    *value = n;
    return true;
}

static bool compare_slug(const char *s, size_t capacity) {
    if (!s[0] || strlen(s) >= capacity) return false;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return false;
    return true;
}

static bool compare_environment_read(const char *path, comparison_t *c) {
    compare_environment_t *e = &c->environment;
    *e = (compare_environment_t){.name = "ideal", .clocks = GOOD, .loss_permille = c->cfg.loss_permille};
    if (!path) return true;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return false; }
    unsigned seen = 0;
    int line_no = 0;
    const char *error = "invalid environment directive, token count, duplicate or integer";
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(f)) { error = "line too long"; goto bad; }
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *tokens[5];
        int count = 0;
        for (char *p = strtok(line, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
            if (count == 5) goto bad;
            tokens[count++] = p;
        }
        if (!count) continue;
        if (!strcmp(tokens[0], "environment")) {
            if (count != 2 || seen & 1 || !compare_slug(tokens[1], sizeof(e->name))) goto bad;
            strcpy(e->name, tokens[1]);
            seen |= 1;
            continue;
        }
        int64_t a, b, target;
        if (!strcmp(tokens[0], "gnss_outage") || !strcmp(tokens[0], "fix_outage")) {
            if (count != 4 || e->n_outages == COMPARE_EVENTS ||
                !compare_integer(tokens[1], 0, c->cfg.duration_s - 1, &a) ||
                !compare_integer(tokens[2], 1, c->cfg.duration_s, &b) || a >= b) goto bad;
            target = -1;
            if (strcmp(tokens[3], "all") && !compare_integer(tokens[3], 0, c->cfg.nodes - 1, &target)) goto bad;
            e->outages[e->n_outages++] = (compare_outage_t){a * SEC, b * SEC, (int)target,
                                                         !strcmp(tokens[0], "gnss_outage") ? 1u : 2u};
            continue;
        }
        if (!strcmp(tokens[0], "interference")) {
            if (count != 3 || seen & 2 || !compare_integer(tokens[1], 1, 60000, &a) ||
                !compare_integer(tokens[2], 0, a, &b)) goto bad;
            e->interference_period_us = a * 1000;
            e->interference_busy_us = b * 1000;
            seen |= 2;
            continue;
        }
        const char *names[] = {"loss_permille", "drift_ppm", "nmea_only", "nmea_lag_us", "nmea_jitter_us",
                               "nmea_told_lag_us", "nmea_error_us"};
        const int highs[] = {1000, 1000, c->cfg.nodes, 999999, 999999, 999999, 999999};
        int field;
        for (field = 0; field < 7 && strcmp(tokens[0], names[field]); field++);
        if (field == 7 || count != 2 || seen & (4u << field) ||
            !compare_integer(tokens[1], 0, highs[field], &a)) goto bad;
        seen |= 4u << field;
        switch (field) {
            case 0: e->loss_permille = (int)a; break;
            case 1: e->clocks.drift_ppm = (double)a; break;
            case 2: e->clocks.nmea_only = (int)a; break;
            case 3: e->clocks.lag_us = a; break;
            case 4: e->clocks.jitter_us = a; break;
            case 5: e->clocks.told_lag_us = a; break;
            case 6: e->clocks.told_err_us = a; break;
        }
    }
    if (ferror(f)) { error = "read failed"; goto bad; }
    if (!(seen & 1)) { error = "environment name is required"; goto bad; }
    if (e->clocks.lag_us < e->clocks.jitter_us || e->clocks.lag_us + e->clocks.jitter_us >= SEC) {
        error = "NMEA lag +/- jitter must stay within one second"; goto bad;
    }
    if (e->interference_period_us) {
        e->interference_phase_us = splitmix(c->seed ^ 0x16c3a92bu) % (uint64_t)e->interference_period_us;
    }
    fclose(f);
    return true;
bad:
    fprintf(stderr, "%s:%d: %s\n", path, line_no, error);
    fclose(f);
    return false;
}

static unsigned compare_gnss_mask(void *ctx, int node, int64_t t) {
    const comparison_t *c = ctx;
    unsigned mask = 0;
    for (int i = 0; i < c->environment.n_outages; i++) {
        const compare_outage_t *x = &c->environment.outages[i];
        if ((x->node == -1 || x->node == node) && t >= x->from_us && t < x->until_us) mask |= x->mask;
    }
    return mask;
}

static bool compare_external_busy(void *ctx, const transmission_t *x) {
    const compare_environment_t *e = &((comparison_t *)ctx)->environment;
    if (!e->interference_busy_us) return false;
    const int64_t period = e->interference_period_us;
    const int64_t phase = (x->start - e->interference_phase_us + period) % period;
    return phase < e->interference_busy_us || x->end - x->start > period - phase;
}

static int compare_state_order(const void *a, const void *b) {
    const compare_state_t *x = a, *y = b;
    if (x->at_us != y->at_us) return (x->at_us > y->at_us) - (x->at_us < y->at_us);
    return x->source - y->source;
}

static bool compare_read(const char *path, compare_config_t *c) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return false; }
    unsigned seen = 0;
    int line_no = 0, largest_link_node = -1;
    const char *error = "invalid directive, token count or integer";
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(f)) { error = "line too long"; goto bad; }
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        char *tokens[7];
        int count = 0;
        for (char *p = strtok(line, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
            if (count == 7) goto bad;
            tokens[count++] = p;
        }
        if (!count) continue;
        if (!strcmp(tokens[0], "name")) {
            if (count != 2 || seen & 1 || strlen(tokens[1]) >= sizeof(c->name)) goto bad;
            for (const char *p = tokens[1]; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') goto bad;
            }
            strcpy(c->name, tokens[1]);
            seen |= 1;
            continue;
        }
        struct { const char *name; int *field; int low, high; unsigned bit; } fields[] = {
            {"nodes", &c->nodes, 2, MAX_NODES, 2},
            {"interval_ms", &c->interval_ms, 100, 60000, 4},
            {"duration_s", &c->duration_s, 1, 3600, 8},
            {"warmup_s", &c->warmup_s, 0, 3599, 16},
            {"loss_permille", &c->loss_permille, 0, 1000, 32},
        };
        bool scalar = false;
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            if (strcmp(tokens[0], fields[i].name)) continue;
            int64_t value;
            if (count != 2 || seen & fields[i].bit ||
                !compare_integer(tokens[1], fields[i].low, fields[i].high, &value)) goto bad;
            *fields[i].field = (int)value;
            seen |= fields[i].bit;
            scalar = true;
            break;
        }
        if (scalar) continue;
        int64_t a, b, d, e;
        if (!strcmp(tokens[0], "link") || !strcmp(tokens[0], "clique")) {
            if (count != 3 || !compare_integer(tokens[1], 0, MAX_NODES - 1, &a) ||
                !compare_integer(tokens[2], 0, MAX_NODES - 1, &b)) goto bad;
            if (a > largest_link_node) largest_link_node = (int)a;
            if (b > largest_link_node) largest_link_node = (int)b;
            if (!strcmp(tokens[0], "link")) {
                if (a == b) goto bad;
                c->links[a][b] = true;
            } else {
                if (a > b) goto bad;
                for (int i = (int)a; i <= b; i++)
                    for (int j = (int)a; j <= b; j++) if (i != j) c->links[i][j] = true;
            }
        } else if (!strcmp(tokens[0], "cut")) {
            if (count != 5 || c->n_cuts == COMPARE_EVENTS ||
                !compare_integer(tokens[1], 0, 3599, &a) || !compare_integer(tokens[2], 1, 3600, &b) ||
                !compare_integer(tokens[3], 0, MAX_NODES - 1, &d) ||
                !compare_integer(tokens[4], 0, MAX_NODES - 1, &e) || a >= b || d == e) goto bad;
            c->cuts[c->n_cuts++] = (compare_cut_t){a * SEC, b * SEC, (int)d, (int)e};
        } else if (!strcmp(tokens[0], "help")) {
            if (count != 4 || c->n_states == COMPARE_EVENTS ||
                !compare_integer(tokens[1], 0, 3599, &a) ||
                !compare_integer(tokens[2], 0, MAX_NODES - 1, &b) ||
                !compare_integer(tokens[3], 0, 1, &d)) goto bad;
            c->states[c->n_states++] = (compare_state_t){.at_us = a * SEC, .source = (int)b, .help = d != 0};
        } else goto bad;
    }
    if (ferror(f)) { error = "read failed"; goto bad; }
    if ((seen & 31) != 31 || c->warmup_s >= c->duration_s) {
        error = "required settings missing or warmup is not shorter than duration"; goto bad;
    }
    if (largest_link_node >= c->nodes) { error = "link outside the roster"; goto bad; }
    for (int i = 0; i < MAX_NODES; i++) {
        for (int j = 0; j < MAX_NODES; j++) {
            if (c->links[i][j] && (i >= c->nodes || j >= c->nodes)) {
                error = "link outside the roster"; goto bad;
            }
        }
    }
    for (int i = 0; i < c->n_cuts; i++) {
        const compare_cut_t *x = &c->cuts[i];
        if (x->from >= c->nodes || x->to >= c->nodes || x->until_us > c->duration_s * SEC ||
            !c->links[x->from][x->to]) {
            error = "cut outside the roster, duration or an existing link"; goto bad;
        }
    }
    qsort(c->states, (size_t)c->n_states, sizeof(c->states[0]), compare_state_order);
    bool state[MAX_NODES] = {false};
    int64_t last[MAX_NODES];
    for (int i = 0; i < MAX_NODES; i++) last[i] = -1;
    for (int i = 0; i < c->n_states; i++) {
        const compare_state_t *x = &c->states[i];
        if (x->source >= c->nodes || x->at_us >= c->duration_s * SEC ||
            x->at_us == last[x->source] || x->help == state[x->source]) {
            error = "help must change a roster member's state at a distinct time within the run"; goto bad;
        }
        state[x->source] = x->help;
        last[x->source] = x->at_us;
    }
    fclose(f);
    return true;
bad:
    fprintf(stderr, "%s:%d: %s\n", path, line_no, error);
    fclose(f);
    return false;
}

static bool compare_link(void *ctx, int from, int to, int64_t t) {
    const comparison_t *c = ctx;
    if (!c->cfg.links[from][to]) return false;
    for (int i = 0; i < c->cfg.n_cuts; i++) {
        const compare_cut_t *x = &c->cfg.cuts[i];
        if (x->from == from && x->to == to && t >= x->from_us && t < x->until_us) return false;
    }
    return true;
}

static bool compare_loss(void *ctx, const transmission_t *x, int receiver) {
    const comparison_t *c = ctx;
    /* A draw belongs to a time and directed link, not to a candidate's number of RNG calls. */
    const uint32_t draw = splitmix(c->seed ^ splitmix((uint32_t)x->start) ^
                                  splitmix((uint32_t)((uint64_t)x->start >> 32)) ^
                                  splitmix((uint32_t)(x->from * MAX_NODES + receiver + 1)));
    return draw % 1000000u < (uint32_t)c->environment.loss_permille * 1000u;
}

static void compare_failure(comparison_t *c, const char *what, int receiver, int source, int64_t t) {
    c->contract_failures++;
    printf("{\"event\":\"contract_failure\",\"check\":\"%s\",\"t_us\":%lld,\"receiver\":%d,\"source\":%d}\n",
           what, (long long)t, receiver, source);
}

static void compare_tx(void *ctx, const transmission_t *x) {
    comparison_t *c = ctx;
    const node_t *n = &c->world->node[x->from];
    c->air_us[x->from] += (uint64_t)(x->end - x->start);
    packet_t p;
    if (pkt_open(x->bytes, x->len, &n->group, &p) != PKT_OK || p.own.slot != x->from) {
        compare_failure(c, "source_identity", -1, x->from, x->start);
        return;
    }
    int64_t utc, err;
    const bool clock = node_utc_us(n, local_us(c->world, x->from, x->start), &utc, &err);
    if (!clock ||
        err + 1000 > node_time_budget_us(n)) {
        compare_failure(c, "timing_budget", -1, x->from, x->start);
    }
    if (clock) {
        const int64_t actual = llabs(utc - (c->world->utc0_ms * 1000 + x->start));
        if (actual > c->clock_error_max_us) c->clock_error_max_us = actual;
        c->clock_bound_exceeded += actual > err;
    }
    printf("{\"event\":\"tx\",\"t_us\":%lld,\"sender\":%d,\"session\":%lu,\"packet\":%u,"
           "\"bytes\":%zu,\"air_us\":%lld}\n", (long long)x->start, x->from,
           (unsigned long)p.session, p.count, x->len, (long long)(x->end - x->start));
}

static int compare_event_at(const comparison_t *c, int source, int64_t t) {
    int event = -1;
    for (int i = 0; i < c->cfg.n_states; i++) {
        if (c->cfg.states[i].source == source && c->cfg.states[i].at_us <= t) event = i;
    }
    return event;
}

static bool compare_superseded(const comparison_t *c, int event, int64_t t) {
    for (int i = event + 1; i < c->cfg.n_states; i++) {
        if (c->cfg.states[i].source == c->cfg.states[event].source && c->cfg.states[i].at_us <= t) return true;
    }
    return false;
}

static void compare_rx(void *ctx, const transmission_t *x, int receiver, bool accepted, const char *reason) {
    comparison_t *c = ctx;
    c->rx_attempts++;
    if (!strcmp(reason, "unreachable")) c->rx_unreachable++;
    else if (!strcmp(reason, "interference")) c->rx_interference++;
    else if (!strcmp(reason, "external_interference")) c->rx_external_interference++;
    else if (!strcmp(reason, "loss")) c->rx_loss++;
    else if (!strcmp(reason, "rejected")) c->rx_rejected++;
    printf("{\"event\":\"rx\",\"t_us\":%lld,\"sender\":%d,\"receiver\":%d,\"accepted\":%s,\"reason\":\"%s\"}\n",
           (long long)x->end, x->from, receiver, accepted ? "true" : "false", reason);
    if (!accepted) return;
    c->rx_accepted++;
    const rider_t *r = &c->world->node[receiver].riders[x->from];
    packet_t p;
    if (pkt_open(x->bytes, x->len, &c->world->node[receiver].group, &p) != PKT_OK ||
        r->via != x->from || r->session != p.session || r->count != p.count) {
        compare_failure(c, "applied_identity", receiver, x->from, x->end);
        return;
    }
    if (x->start < c->last_update[receiver][x->from] || fix_ms(r) < c->last_fix[receiver][x->from]) {
        compare_failure(c, "update_order", receiver, x->from, x->end);
    }
    c->last_update[receiver][x->from] = x->start;
    c->last_fix[receiver][x->from] = fix_ms(r);
    const int64_t warmup = c->cfg.warmup_s * SEC;
    const int64_t before = c->last_rx[receiver][x->from] > warmup ? c->last_rx[receiver][x->from] : warmup;
    if (x->end > before && x->end - before > c->longest_gap_us) c->longest_gap_us = x->end - before;
    c->last_rx[receiver][x->from] = x->end;
    const bool help = r->rec.state == STATE_HELP;
    /* Only this observer knows the originating scenario event; no event id enters a node. */
    const int event = compare_event_at(c, x->from, x->start);
    if (event >= 0) {
        compare_state_t *e = &c->cfg.states[event];
        if (e->at_us >= warmup && e->help == help && !e->delivered[receiver] &&
            !compare_superseded(c, event, x->end)) {
            e->delivered[receiver] = true;
            c->state_delays[c->n_state_delays++] = (double)(x->end - e->at_us) / 1000;
        }
    }
    printf("{\"event\":\"applied\",\"t_us\":%lld,\"created_us\":%lld,\"receiver\":%d,\"source\":%d,"
           "\"via\":%d,\"session\":%lu,\"packet\":%u,\"fix_time_us\":%lld,\"help\":%d,\"state\":\"%s\",\"state_event\":",
           (long long)x->end, (long long)x->start, receiver, x->from, x->from,
           (unsigned long)p.session, p.count, (long long)(fix_ms(r) * 1000), help, help ? "help" : "normal");
    if (event >= 0) printf("%d", event); else printf("null");
    puts("}");
}

static bool compare_path(comparison_t *c, int source, int receiver, int64_t t) {
    bool seen[MAX_NODES] = {false};
    int queue[MAX_NODES], begin = 0, end = 0;
    queue[end++] = source;
    seen[source] = true;
    while (begin < end) {
        const int at = queue[begin++];
        for (int next = 0; next < c->cfg.nodes; next++) {
            if (seen[next] || !compare_link(c, at, next, t)) continue;
            if (next == receiver) return true;
            seen[next] = true;
            queue[end++] = next;
        }
    }
    return false;
}

static void compare_sample(comparison_t *c, int64_t t) {
    for (int receiver = 0; receiver < c->cfg.nodes; receiver++) {
        int64_t utc = 0;
        const bool clock = node_utc_ms(&c->world->node[receiver], local_us(c->world, receiver, t), &utc);
        for (int source = 0; source < c->cfg.nodes; source++) {
            if (source == receiver) continue;
            const rider_t *r = &c->world->node[receiver].riders[source];
            const bool known = r->known && fix_ms(r);
            const bool path = compare_path(c, source, receiver, t);
            c->samples++;
            c->unseen += !known;
            c->clock_unknown += !clock;
            c->path_unavailable += !path;
            printf("{\"event\":\"sample\",\"t_us\":%lld,\"receiver\":%d,\"source\":%d,\"known\":%s,"
                   "\"clock_known\":%s,\"path_available\":%s,\"age_ms\":", (long long)t,
                   receiver, source, known ? "true" : "false", clock ? "true" : "false", path ? "true" : "false");
            if (known && clock) {
                assert(c->n_ages < c->age_capacity);
                const double age = (double)(utc - fix_ms(r));
                c->ages[c->n_ages++] = age;
                printf("%.3f", age);
            } else printf("null");
            puts("}");
        }
    }
}

static void compare_percentile(const double *values, size_t count, size_t percentile) {
    if (count) printf("%.3f", values[(count * percentile + 99) / 100 - 1]); else printf("null");
}

static int compare_run(const char *path, const char *seed_text, const char *environment_path) {
    static comparison_t c;
    static world_t w;
    int64_t seed;
    if (!compare_integer(seed_text, 1, UINT32_MAX, &seed)) {
        fprintf(stderr, "comparison seed must be an integer from 1 to %lu\n", (unsigned long)UINT32_MAX);
        return 2;
    }
    if (!compare_read(path, &c.cfg)) return 2;
    c.seed = (uint32_t)seed;
    if (!compare_environment_read(environment_path, &c)) return 2;
    c.world = &w;
    const scenario_t sc = {.nodes = c.cfg.nodes, .interval_ms = (uint32_t)c.cfg.interval_ms,
                           .duration_us = c.cfg.duration_s * SEC, .seed = c.seed, .groups = 1,
                           .clocks = c.environment.clocks};
    world_init(&w, &sc, 1792670400000ll);
    w.ctx = &c;
    w.link_at = compare_link;
    w.packet_lost = compare_loss;
    w.external_busy = compare_external_busy;
    w.gnss_mask = compare_gnss_mask;
    w.on_tx = compare_tx;
    w.on_rx = compare_rx;
    c.age_capacity = (size_t)(c.cfg.duration_s - c.cfg.warmup_s) * (size_t)c.cfg.nodes * (size_t)(c.cfg.nodes - 1);
    c.ages = malloc(c.age_capacity * sizeof(*c.ages));
    if (!c.ages) { perror("comparison ages"); return 2; }
    printf("{\"event\":\"run\",\"schema\":1,\"candidate\":\"current-v3\",\"evidence\":\"simulation\","
           "\"scenario\":\"%s\",\"seed\":%lu,\"nodes\":%d,\"interval_ms\":%d,\"duration_s\":%d,"
           "\"warmup_s\":%d,\"loss_permille\":%d,\"phy\":\"SF7/250\",\"packet_bytes\":%d,"
           "\"clock_model\":\"constant crystal drift, PPS/NMEA as configured\",\"driver\":\"bypassed\","
           "\"receiver\":\"C node, not phone or map\",\"motion\":\"synthetic circular rides\","
           "\"step_us\":%d,\"sample_period_us\":1000000,\"utc0_ms\":%lld,\"listen_one_in\":32,"
           "\"checks_executed\":[\"source_identity\",\"applied_identity\",\"update_order\",\"timing_budget\"],"
           "\"checks_not_exercised\":[\"relayed_identity\",\"relay_expiry\",\"old_state_replay\","
           "\"fix_age_integrity\",\"full_slot_containment\",\"rolling_duty_limit\"],", c.cfg.name, (unsigned long)c.seed,
           c.cfg.nodes, c.cfg.interval_ms, c.cfg.duration_s, c.cfg.warmup_s, c.environment.loss_permille, PKT_LEN(1),
           STEP_US, (long long)w.utc0_ms);
    const compare_environment_t *e = &c.environment;
    printf("\"environment\":\"%s\",\"drift_ppm\":%.0f,\"nmea_only\":%d,\"nmea_lag_us\":%lld,"
           "\"nmea_jitter_us\":%lld,\"nmea_told_lag_us\":%lld,\"nmea_error_us\":%lld,"
           "\"interference_period_us\":%lld,\"interference_busy_us\":%lld,\"interference_phase_us\":%lld,"
           "\"radio_model\":\"half duplex; any audible overlap loses packet; no capture; constant -90 dBm; immediate TX\","
           "\"outages\":[", e->name, e->clocks.drift_ppm, e->clocks.nmea_only, (long long)e->clocks.lag_us,
           (long long)e->clocks.jitter_us, (long long)e->clocks.told_lag_us, (long long)e->clocks.told_err_us,
           (long long)e->interference_period_us, (long long)e->interference_busy_us, (long long)e->interference_phase_us);
    for (int i = 0; i < e->n_outages; i++) {
        const compare_outage_t *x = &e->outages[i];
        printf("%s{\"type\":\"%s\",\"from_us\":%lld,\"until_us\":%lld,\"node\":%d}", i ? "," : "",
               x->mask == 1 ? "gnss" : "fix", (long long)x->from_us, (long long)x->until_us, x->node);
    }
    printf("],\"node_clocks\":[");
    for (int i = 0; i < c.cfg.nodes; i++) {
        printf("%s{\"node\":%d,\"drift_ppm\":%.6f,\"offset_us\":%lld}", i ? "," : "", i,
               w.clock[i].drift * 1e6, (long long)w.clock[i].offset_us);
    }
    puts("]}");
    int next_state = 0;
    for (int64_t t = 0; t < sc.duration_us; t += STEP_US) {
        while (next_state < c.cfg.n_states && c.cfg.states[next_state].at_us == t) {
            const compare_state_t *e = &c.cfg.states[next_state];
            node_set_help(&w.node[e->source], e->help);
            printf("{\"event\":\"state\",\"t_us\":%lld,\"source\":%d,\"help\":%d,\"state_event\":%d}\n",
                   (long long)t, e->source, e->help, next_state++);
        }
        world_step(&w, t);
        if (t >= c.cfg.warmup_s * SEC && t % SEC == SEC / 2) compare_sample(&c, t);
    }
    for (int receiver = 0; receiver < c.cfg.nodes; receiver++) {
        for (int source = 0; source < c.cfg.nodes; source++) {
            if (source == receiver) continue;
            const int64_t last = c.last_rx[receiver][source] > c.cfg.warmup_s * SEC ?
                                     c.last_rx[receiver][source] : c.cfg.warmup_s * SEC;
            if (sc.duration_us - last > c.longest_gap_us) c.longest_gap_us = sc.duration_us - last;
        }
    }
    int expected = 0, delivered = 0, superseded = 0, pending = 0;
    for (int i = 0; i < c.cfg.n_states; i++) {
        const compare_state_t *e = &c.cfg.states[i];
        if (e->at_us < c.cfg.warmup_s * SEC) continue;
        for (int receiver = 0; receiver < c.cfg.nodes; receiver++) {
            if (receiver == e->source) continue;
            expected++;
            if (e->delivered[receiver]) delivered++;
            else if (compare_superseded(&c, i, sc.duration_us)) superseded++;
            else pending++;
        }
    }
    uint64_t total_air = 0, max_air = 0;
    unsigned long unsure = 0;
    for (int i = 0; i < c.cfg.nodes; i++) {
        total_air += c.air_us[i];
        if (c.air_us[i] > max_air) max_air = c.air_us[i];
        unsure += w.node[i].tx_unsure;
    }
    qsort(c.ages, c.n_ages, sizeof(*c.ages), by_value);
    qsort(c.state_delays, (size_t)c.n_state_delays, sizeof(c.state_delays[0]), by_value);
    printf("{\"event\":\"summary\",\"nodes\":%d,\"age_samples\":%llu,\"age_known_samples\":%zu,\"unseen_samples\":%llu,"
           "\"clock_unknown_samples\":%llu,\"unseen_fraction\":%.9f,\"clock_unknown_fraction\":%.9f,"
           "\"path_unavailable_fraction\":%.9f,\"age_p50_ms\":", c.cfg.nodes,
           (unsigned long long)c.samples, c.n_ages, (unsigned long long)c.unseen, (unsigned long long)c.clock_unknown,
           (double)c.unseen / c.samples, (double)c.clock_unknown / c.samples, (double)c.path_unavailable / c.samples);
    compare_percentile(c.ages, c.n_ages, 50);
    printf(",\"age_p95_ms\":"); compare_percentile(c.ages, c.n_ages, 95);
    printf(",\"gap_max_ms\":%.3f,\"state_expected\":%d,\"state_delivered\":%d,\"state_superseded\":%d,"
           "\"state_pending\":%d,\"state_delay_samples\":%d,\"state_delay_p50_ms\":",
           (double)c.longest_gap_us / 1000, expected, delivered, superseded, pending, c.n_state_delays);
    compare_percentile(c.state_delays, (size_t)c.n_state_delays, 50);
    printf(",\"state_delay_p95_ms\":"); compare_percentile(c.state_delays, (size_t)c.n_state_delays, 95);
    printf(",\"tx_airtime_max_fraction\":%.9f,\"tx_airtime_total_us\":%llu,\"tx_count\":%d,"
           "\"rx_accepted\":%d,\"tx_unsure\":%lu,\"contract_failures\":%d,\"rx_attempts\":%llu,"
           "\"rx_unreachable\":%llu,\"rx_interference\":%llu,\"rx_external_interference\":%llu,"
           "\"rx_loss\":%llu,\"rx_rejected\":%llu,\"clock_error_max_us\":%lld,\"clock_bound_exceeded\":%llu,"
           "\"tx_airtime_us\":[",
           (double)max_air / sc.duration_us, (unsigned long long)total_air, w.sent, c.rx_accepted, unsure, c.contract_failures,
           (unsigned long long)c.rx_attempts, (unsigned long long)c.rx_unreachable, (unsigned long long)c.rx_interference,
           (unsigned long long)c.rx_external_interference, (unsigned long long)c.rx_loss, (unsigned long long)c.rx_rejected,
           (long long)c.clock_error_max_us, (unsigned long long)c.clock_bound_exceeded);
    for (int i = 0; i < c.cfg.nodes; i++) printf("%s%llu", i ? "," : "", (unsigned long long)c.air_us[i]);
    puts("]}");
    free(c.ages);
    return c.contract_failures ? 1 : 0;
}

/* ---- serve: simulated boards for the app --------------------------------------------- */

typedef struct {
    int listener;
    int client;            /* -1 while no phone is connected */
    char in[512];
    size_t in_len;
    api_t api;
    int64_t next_status;
} board_t;

static board_t s_boards[MAX_NODES];
static int s_n_boards;

static int sim_dbm(uint8_t level) { return 22 - 2 * (level & 3); }

static int64_t real_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * SEC + ts.tv_nsec / 1000;
}

static void drop(board_t *b, int i) {
    close(b->client);
    b->client = -1;
    b->in_len = 0;
    printf("board %d: phone gone\n", i);
}

/* One line to the phone, newline-terminated; a phone that cannot take it is dropped. */
static void send_line(void *ctx, const char *line) {
    board_t *b = ctx;
    if (b->client < 0) return;
    char out[API_LINE + 2];
    const int n = snprintf(out, sizeof(out), "%s\n", line);
    if (send(b->client, out, (size_t)n, MSG_NOSIGNAL) != n) drop(b, (int)(b - s_boards));
}

/* One line from a phone to a simulated board, and then what run.c does after a command on a real
 * board, short of what only Bluetooth can do: a mode asked for its radio takes at once, as a board's
 * does once radio_apply succeeds, and the node is told (D15). */
static void serve_line(board_t *b, char *line, int64_t now_us) {
    api_command(&b->api, line, now_us, now_us, send_line, b);
    if (b->api.forget) send_line(b, "ERR FORGET is for Bluetooth");
    if (b->api.radio_changed) node_phy_applied(b->api.node, &b->api.node->cfg.phy);
    b->api.forget = b->api.restart = b->api.bootloader = b->api.radio_changed = false;
}

/* A packet a board took: a line to the phone for each rider it brought a newer record of, its
 * sender first, and our own state newly carried with the status, at once, as a board sends them
 * (run.c, D16). */
static void on_taken(void *ctx, int node, unsigned slot, int64_t t) {
    world_t *w = ctx;
    board_t *b = &s_boards[node];
    node_t *n = &w->node[node];
    const bool status = n->echo_news;
    n->echo_news = false;
    if (b->client < 0) return;
    char line[API_LINE];
    for (unsigned i = 0; i <= NODE_SLOTS; i++) {
        const unsigned s = i == 0 ? slot : i - 1;
        if ((i == 0 || s != slot) && (n->rx_changed >> s & 1)) {
            node_format_rider(n, s, t, line, sizeof(line));
            send_line(b, line);
        }
    }
    if (status) {
        api_status(&b->api, t, line, sizeof(line));
        send_line(b, line);
    }
}

/* How many of the others hold rider k as calling for help: heard from itself, or told. */
static int know_of_call(const world_t *w, int nodes, int k) {
    int count = 0;
    for (int i = 0; i < nodes; i++) {
        const rider_t *r = &w->node[i].riders[w->slot_of[k]];
        count += i != k && r->known && r->rec.state == STATE_HELP;
    }
    return count;
}

static int listen_on(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        fprintf(stderr, "cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
        exit(1);
    }
    return fd;
}

static void serve(int n, int port) {
    static world_t w;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const scenario_t sc = {n, 2000, 0, 0, (uint32_t)now.tv_sec, 0, GOOD, 0, false};
    /* on a whole second, as GNSS time is, so the PPS edges and the fixes pair up */
    world_init(&w, &sc, (int64_t)now.tv_sec * 1000);
    w.on_taken = on_taken;
    w.ctx = &w;
    s_n_boards = n;
    for (int i = 0; i < n; i++) {
        s_boards[i] = (board_t){.listener = listen_on(port + i), .client = -1,
                                .api = {.node = &w.node[i], .board = "sim", .reset = "poweron",
                                        .power = 3, .tx_dbm = sim_dbm}};
        printf("board %d (slot %d): phone API on 127.0.0.1:%d\n", i, i, port + i);
    }
    fflush(stdout);
    const int64_t start = real_us();
    int64_t t = 0;
    for (;;) {
        const int64_t now_us = real_us() - start;
        for (; t <= now_us; t += STEP_US) world_step(&w, t);

        struct pollfd fds[2 * MAX_NODES];
        for (int i = 0; i < n; i++) {
            fds[2 * i] = (struct pollfd){.fd = s_boards[i].listener, .events = POLLIN};
            fds[2 * i + 1] = (struct pollfd){.fd = s_boards[i].client, .events = POLLIN};
        }
        if (poll(fds, (nfds_t)(2 * n), 1) <= 0) goto status;
        for (int i = 0; i < n; i++) {
            board_t *b = &s_boards[i];
            if (fds[2 * i].revents & POLLIN) {
                const int c = accept(b->listener, NULL, NULL);
                if (b->client >= 0) {
                    close(c); /* one phone per board, as over Bluetooth */
                } else {
                    b->client = c;
                    b->next_status = 0;
                    printf("board %d: phone connected\n", i);
                }
            }
            if (b->client < 0 || !(fds[2 * i + 1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = recv(b->client, b->in + b->in_len, sizeof(b->in) - 1 - b->in_len, 0);
            if (got <= 0) {
                drop(b, i);
                continue;
            }
            b->in_len += (size_t)got;
            char *nl;
            while (b->client >= 0 && (nl = memchr(b->in, '\n', b->in_len))) {
                *nl = '\0';
                if (nl > b->in && nl[-1] == '\r') nl[-1] = '\0';
                if (b->in[0]) serve_line(b, b->in, local_us(&w, i, t));
                const size_t used = (size_t)(nl - b->in) + 1;
                memmove(b->in, nl + 1, b->in_len - used);
                b->in_len -= used;
            }
            if (b->in_len == sizeof(b->in) - 1) drop(b, i); /* a line too long to be ours */
        }
    status:
        /* the status unasked, as a board sends it: on connecting and every 10 s */
        for (int i = 0; i < n; i++) {
            board_t *b = &s_boards[i];
            if (b->client < 0 || t < b->next_status) continue;
            char line[API_LINE];
            api_status(&b->api, local_us(&w, i, t), line, sizeof(line));
            send_line(b, line);
            b->next_status = t + 10 * SEC;
        }
        fflush(stdout);
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--compare") == 0) {
        if (argc != 4 && argc != 5) {
            fprintf(stderr, "usage: sim --compare SCENARIO_FILE SEED [ENVIRONMENT_FILE]\n");
            return 2;
        }
        return compare_run(argv[2], argv[3], argc == 5 ? argv[4] : NULL);
    }
    if (argc >= 3 && strcmp(argv[1], "serve") == 0) {
        const int n = atoi(argv[2]);
        if (n < 2 || n > MAX_NODES) {
            fprintf(stderr, "serve: 2 to %d boards\n", MAX_NODES);
            return 2;
        }
        serve(n, argc >= 4 ? atoi(argv[3]) : 4403);
        return 0;
    }
    if (argc == 4) {
        const scenario_t sc = {atoi(argv[1]), (uint32_t)atoi(argv[2]), atoll(argv[3]) * SEC, 0, 42, 1, GOOD, 0, false};
        const outcome_t o = run(&sc, true);
        printf("%d riders every %u ms for %s s: %d sent, %d collided (%d within the group), %.1f %% delivered, "
               "fix age shown p50 %.1f s, p95 %.1f s, %lu slots passed over unsure of the time\n", sc.nodes,
               sc.interval_ms, argv[3], o.sent, o.collisions, o.within, 100 * o.delivered, o.age_p50, o.age_p95,
               o.unsure);
        return 0;
    }

    /* Three riders, a slot each every 2 s: each hears the others every frame. */
    const scenario_t three = {3, 2000, 600 * SEC, 0, 1, 1, GOOD, 0, false};
    outcome_t o = run(&three, false);
    printf("sim: 3 riders every 2 s: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s, "
           "the longest gap %.0f s\n", 100 * o.delivered, o.collisions, o.age_p50, o.age_p95, o.gap);
    /* a gap is a frame, or a few where a rider listened in its slot: never more without loss */
    assert(o.delivered > 0.99 && o.collisions == 0 && o.age_p95 <= 3.5 && o.gap >= 2 && o.gap <= 8);

    /* With 10 % random loss on top, still everyone, a little older. */
    const scenario_t lossy = {3, 2000, 600 * SEC, 0.10, 2, 1, GOOD, 0, false};
    o = run(&lossy, false);
    printf("sim: 3 riders, 10 %% random loss: %.1f %% delivered, fix age p50 %.1f s, p95 %.1f s, the longest gap "
           "%.0f s\n", 100 * o.delivered, o.age_p50, o.age_p95, o.gap);
    assert(o.delivered > 0.85 && o.delivered < 0.95 && o.age_p95 <= 5.5);

    /* Twenty riders every 2 s: random access lost 45 % of them to collisions (M5); slots lose none. */
    const scenario_t twenty = {20, 2000, 600 * SEC, 0, 3, 1, GOOD, 0, false};
    o = run(&twenty, false);
    printf("sim: 20 riders every 2 s in slots: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s, "
           "the longest gap %.0f s (random access: 55 %%, p95 8.5 s)\n", 100 * o.delivered, o.collisions,
           o.age_p50, o.age_p95, o.gap);
    assert(o.delivered > 0.99 && o.collisions == 0 && o.age_p95 <= 3.5);

    /* Real clocks: crystals 20 ppm off, boards started seconds apart, half of them L1s without PPS
     * whose NMEA comes 120 ms after the second, give or take 5 ms, and are told so. */
    const clocks_t mixed = {.drift_ppm = 20, .nmea_only = 10, .lag_us = 120000, .jitter_us = 5000,
                            .told_lag_us = 120000, .told_err_us = 10000};
    const scenario_t real = {20, 2000, 600 * SEC, 0, 5, 1, mixed, 0, false};
    o = run(&real, false);
    printf("sim: 20 riders, half without PPS, crystals 20 ppm off: %.1f %% delivered, %d collisions\n",
           100 * o.delivered, o.collisions);
    assert(o.delivered > 0.99 && o.collisions == 0 && o.unsure == 0);

    /* A tunnel: three riders lose their GNSS for 15 minutes. v3 held their slots about 10 minutes,
     * at 50 ppm against 31 ms, and then went quiet. Now the packets of the seventeen on PPS keep
     * their clocks set (12, 2026-11-14): they hold their slots throughout. Nobody collides. */
    const clocks_t tunnel = {.drift_ppm = 20, .lag_us = 100000, .told_err_us = 100000, .dark = 3,
                             .dark_from_us = 60 * SEC, .dark_us = 900 * SEC};
    const scenario_t dark = {20, 2000, 1200 * SEC, 0, 6, 1, tunnel, 0, false};
    o = run(&dark, false);
    unsigned long quiet = 0, after = 0;
    for (int i = 17; i < 20; i++) {
        quiet += s_run.node[i].tx_unsure;
        after += s_run.sent_by[i];
    }
    printf("sim: 3 of 20 riders 15 min without GNSS, the others on PPS: they passed over %lu slots unsure of "
           "their time, and sent %lu packets; %d collisions\n", quiet, after, o.collisions);
    assert(o.collisions == 0 && quiet == 0 && after > 3 * 550);

    /* An L1 told nothing of its NMEA's lag sends 100 ms late, into the next rider's slot: why E0
     * measures the lag. Told it is unsure, as boards are by default (100 ms), v3 kept it quiet in a
     * group this size; now the packets of the riders on PPS set its clock, and it keeps its slot. */
    const clocks_t honest = {.nmea_only = 1, .lag_us = 100000, .jitter_us = 5000, .told_err_us = 100000};
    const scenario_t unsure = {20, 2000, 600 * SEC, 0, 7, 1, honest, 0, false};
    o = run(&unsure, false);
    printf("sim: an L1 told nothing of its NMEA's lag, and unsure of it, among riders on PPS: %d silent, %d "
           "collisions\n", o.silent, o.within);
    assert(o.silent == 0 && o.within == 0);

    /* sim serve's boards take MODE as boards do (D15): the phone's command, through the same
     * handling as serve's, and their radios take the mode, so they go on sending, in it. */
    {
        static world_t v;
        const scenario_t three = {3, 2000, 0, 0, 16, 1, GOOD, 0, false};
        world_init(&v, &three, 1792670400000ll);
        int64_t t = 0;
        for (; t < 20 * SEC; t += STEP_US) world_step(&v, t);
        const int before = v.sent;
        for (int i = 0; i < 3; i++) {
            board_t b = {.client = -1, .api = {.node = &v.node[i], .board = "sim", .reset = "poweron", .power = 3,
                                               .tx_dbm = sim_dbm}};
            char mode[] = "MODE 9 250";
            serve_line(&b, mode, local_us(&v, i, t));
        }
        for (const int64_t end = t + 20 * SEC; t < end; t += STEP_US) world_step(&v, t);
        unsigned long held = 0;
        for (int i = 0; i < 3; i++) held += v.node[i].tx_held;
        printf("sim: serve's boards told MODE 9 250 by their phones: %d sent in 20 s before, %d after, %lu slots held\n",
               before, v.sent - before, held);
        assert(before > 20 && v.sent - before > 20 && held == 0 && v.node[0].phy.sf == 9);
    }

    /* sim serve tells a phone of a new answer to a call for help at once, as a board does (D16):
     * the caller's phone gets the status with the answer in it, a bystander's the caller's line with
     * its answerer, with the packet that brought it rather than with the next status. Two phones on
     * socket pairs; no status but news is sent here. */
    {
        static world_t q;
        const scenario_t three = {3, 2000, 0, 0, 17, 1, GOOD, 0, false};
        world_init(&q, &three, 1792670400000ll);
        q.on_taken = on_taken;
        q.ctx = &q;
        int phone[2][2];
        for (int i = 0; i < 3; i++) {
            s_boards[i] = (board_t){.listener = -1, .client = -1, .api = {.node = &q.node[i], .board = "sim",
                                    .reset = "poweron", .power = 3, .tx_dbm = sim_dbm}};
        }
        for (int k = 0; k < 2; k++) {
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, phone[k]) == 0);
            assert(fcntl(phone[k][1], F_SETFL, O_NONBLOCK) == 0);
            s_boards[k].client = phone[k][0];
        }
        int64_t t = 0;
        for (; t < 10 * SEC; t += STEP_US) world_step(&q, t);
        node_set_help(&q.node[0], true);
        for (; __builtin_popcount(q.node[0].echo_by) < 2; t += STEP_US) {
            world_step(&q, t);
            assert(t < 30 * SEC);
        }
        static char got[2][1 << 16];
        for (int k = 0; k < 2; k++) {
            size_t n = 0;
            for (ssize_t r; (r = read(phone[k][1], got[k] + n, sizeof(got[k]) - 1 - n)) > 0;) n += (size_t)r;
            got[k][n] = '\0';
            close(phone[k][0]);
            close(phone[k][1]);
            s_boards[k].client = -1;
        }
        const bool caller = strstr(got[0], "STATUS ") && strstr(got[0], " heard=1 state=1 ");
        const bool bystander = strstr(got[1], "RIDER slot=0 ") && strstr(got[1], " state=1 ");
        printf("sim: serve tells the phones of an answer to a call for help with the packet that brings it: the "
               "caller's in its status %s, a bystander's in the caller's line %s\n", caller ? "yes" : "NO",
               bystander ? "yes" : "NO");
        assert(caller && bystander);
    }

    /* A mode changed under a group (D15): ten riders on SF11/250, whose 370.688 ms packet is longer
     * than their slots are apart, so silent; then all asked for SF7/250, which their radios take a
     * frame later. Until then nothing goes out, in either mode; after, nothing collides within the
     * group, and each packet lasts as long as the mode it went with. */
    {
        static world_t m;
        const scenario_t ten = {10, 2000, 0, 0, 15, 1, GOOD, 0, false};
        world_init(&m, &ten, 1792670400000ll);
        phy_t slow = PHY_V0;
        slow.sf = 11;
        for (int i = 0; i < 10; i++) {
            m.node[i].cfg.phy = slow;
            node_phy_applied(&m.node[i], &slow);
        }
        int64_t t = 0;
        for (; t < 20 * SEC; t += STEP_US) world_step(&m, t);
        const int silent = m.sent;
        for (int i = 0; i < 10; i++) m.node[i].cfg.phy = PHY_V0;
        for (; t < 22 * SEC; t += STEP_US) world_step(&m, t);
        const int before = m.sent;
        unsigned long held = 0;
        for (int i = 0; i < 10; i++) {
            held += m.node[i].tx_held;
            node_phy_applied(&m.node[i], &PHY_V0);
        }
        for (; t < 80 * SEC; t += STEP_US) world_step(&m, t);
        printf("sim: a mode changed under ten riders, their radios a frame late to take it: %d sent before, %lu slots "
               "held, %d sent after, %d collisions within the group\n", before, held, m.sent - before, m.within);
        assert(silent == 0 && before == 0 && held >= 10 && m.sent - before > 10 * 25 && m.within == 0);
    }

    /* E0 in the model (09): an L1 told nothing of its NMEA's 100 ms lag, in a group of three, its
     * slots 500 ms apart, so nothing collides: the other boards hear it begin 100 ms after its
     * moment, give or take its NMEA's jitter, and each other on time; each board's own RF starts
     * within the simulator's step of a millisecond. The model has no driver: on a board, E0 measures
     * the rest. */
    {
        const clocks_t late = {.nmea_only = 1, .lag_us = 100000, .jitter_us = 5000, .told_err_us = 10000};
        const scenario_t three_late = {3, 2000, 120 * SEC, 0, 14, 1, late, 0, false};
        run(&three_late, false);
        const spread_t *l1 = &s_run.node[1].riders[s_run.slot_of[0]].offset, *pps = &s_run.node[1].riders[s_run.slot_of[2]].offset;
        const spread_t *lead = &s_run.node[2].tx_lead;
        printf("sim: E0 in the model: an L1 told nothing of its NMEA's lag is heard %.1f ms late on average (%.1f to "
               "%.1f), a board with PPS %.1f ms; RF %.2f ms after its moment at most\n", l1->sum_us / 1000.0 / l1->n,
               l1->min_us / 1000.0, l1->max_us / 1000.0, pps->sum_us / 1000.0 / pps->n, lead->max_us / 1000.0);
        assert(l1->n > 30 && l1->sum_us / l1->n > 94000 && l1->sum_us / l1->n < 106000);
        assert(pps->n > 30 && pps->max_us >= 0 && pps->max_us <= 1000 && lead->max_us <= 1000);
    }

    /* "Need help" (01, help delivery): each of twenty riders calls in turn, with 10 % of packets
     * lost; how long until the caller sees its call carried by another, and until every other
     * rider has it, heard or told. */
    {
        static world_t h;
        const scenario_t helpers = {20, 2000, 0, 0.10, 9, 1, GOOD, 0, false};
        world_init(&h, &helpers, 1792670400000ll);
        double first[20], all[20];
        int64_t t = 0;
        for (; t < 20 * SEC; t += STEP_US) world_step(&h, t);
        for (int k = 0; k < 20; k++) {
            node_set_help(&h.node[k], true);
            const int64_t asked = t;
            int64_t carried = 0;
            for (; know_of_call(&h, 20, k) < 19 || !carried; t += STEP_US) {
                world_step(&h, t);
                if (h.node[k].echo_by && !carried) carried = t;
                assert(t - asked < 60 * SEC);
            }
            first[k] = (double)(carried - asked) / SEC;
            all[k] = (double)(t - asked) / SEC;
            node_set_help(&h.node[k], false);
            /* the next call at any moment of a frame, once this one is called off everywhere */
            for (const int64_t end = t + 10 * SEC + rnd(&h.rng) % 2000 * 1000; t < end; t += STEP_US) world_step(&h, t);
        }
        qsort(first, 20, sizeof(double), by_value);
        qsort(all, 20, sizeof(double), by_value);
        printf("sim: a call for help among 20 riders, 10 %% of packets lost: the caller sees it carried within "
               "%.1f s, everyone has it within %.1f s (the median; at worst %.1f and %.1f s)\n", first[10], all[10],
               first[19], all[19]);
        assert(first[19] < 6 && all[19] < 10);
    }

    /* Several calls at once: a packet carries one changed state first, so h callers take turns in
     * every other rider's packets. */
    for (int h = 1; h <= 4; h *= 2) {
        static world_t m;
        const scenario_t many = {20, 2000, 0, 0.10, 12, 1, GOOD, 0, false};
        world_init(&m, &many, 1792670400000ll);
        double all[10 * 4];
        int n = 0;
        int64_t t = 0;
        for (; t < 20 * SEC; t += STEP_US) world_step(&m, t);
        for (int round = 0; round < 10; round++) {
            int callers[4];
            int64_t done[4] = {0};
            for (int c = 0; c < h; c++) node_set_help(&m.node[callers[c] = (round * 7 + c * 5) % 20], true);
            const int64_t asked = t;
            for (int left = h; left > 0; t += STEP_US) {
                world_step(&m, t);
                for (int c = 0; c < h; c++) {
                    if (!done[c] && know_of_call(&m, 20, callers[c]) >= 19) left -= (done[c] = t) > 0;
                }
                assert(t - asked < 120 * SEC);
            }
            for (int c = 0; c < h; c++) {
                all[n++] = (double)(done[c] - asked) / SEC;
                node_set_help(&m.node[callers[c]], false);
            }
            for (const int64_t end = t + 10 * SEC + rnd(&m.rng) % 2000 * 1000; t < end; t += STEP_US) world_step(&m, t);
        }
        qsort(all, (size_t)n, sizeof(double), by_value);
        printf("sim: %d call%s for help at once among 20 riders, 10 %% of packets lost: all 19 have each within "
               "%.1f s (the median; at worst %.1f s)\n", h, h > 1 ? "s" : "", all[n / 2], all[n - 1]);
        assert(all[n - 1] < 10);
    }

    /* A column of twenty on one road, spreading out to some 9 km in 20 minutes, its radios in reach
     * for sure within 1 km, as in forest (06: out of range means stale, not lost). Rider 6 stops
     * after ten minutes for five, and calls for help. v3: those in reach knew at once, those behind
     * as they came by, those ahead never. Now each rider carries others along, so the call and
     * the positions travel down the column past the radios' reach. */
    {
        static world_t c;
        const scenario_t road = {20, 2000, 1200 * SEC, 0, 10, 1, GOOD, 1000, true};
        world_init(&c, &road, 1792670400000ll);
        static double near[20 * 19 * 1200], far[20 * 19 * 1200], every[20 * 19 * 1200];
        size_t n_near = 0, n_far = 0, n_every = 0, over30 = 0;
        int quick = 0, by_end = 0;
        for (int64_t t = 0; t < road.duration_us; t += STEP_US) {
            world_step(&c, t);
            if (t == STOP_FROM) node_set_help(&c.node[STOPPER], true);
            if (t == STOP_FROM + 10 * SEC) quick = know_of_call(&c, 20, STOPPER);
            if (t == STOP_FROM + STOP_FOR) {
                by_end = know_of_call(&c, 20, STOPPER);
                node_set_help(&c.node[STOPPER], false);
            }
            if (t % SEC != SEC / 2 || t < 20 * SEC) continue;
            for (int i = 0; i < 20; i++) {
                int64_t utc;
                if (!node_utc_ms(&c.node[i], local_us(&c, i, t), &utc)) continue;
                for (int j = 0; j < 20; j++) {
                    if (j == i) continue;
                    const rider_t *r = &c.node[i].riders[c.slot_of[j]];
                    const double age = r->known ? (double)(utc - fix_ms(r)) / 1000 : 1e9, d = distance_m(&c, i, j);
                    if (d <= road.range_m) near[n_near++] = age;
                    if (d >= 1.5 * road.range_m) far[n_far++] = age;
                    every[n_every++] = age;
                    over30 += age > 30;
                }
            }
        }
        double spread = 0;
        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 20; j++) spread = fmax(spread, distance_m(&c, i, j));
        }
        qsort(near, n_near, sizeof(double), by_value);
        qsort(far, n_far, sizeof(double), by_value);
        qsort(every, n_every, sizeof(double), by_value);
        printf("sim: a column of 20 spreading to %.1f km, radios reaching 1 km: in reach, fixes %.1f s old at the "
               "95th percentile; out of it, told by others, %.0f s old at the median. A call for help from a "
               "stopped rider: %d have it within 10 s, %d of 19 before it rides on. Every pair, out of reach or not: "
               "%.0f s old at the median, %.0f s at the 95th percentile, over 30 s old %.0f %% of the time\n",
               spread / 1000, near[n_near * 95 / 100], far[n_far / 2], quick, by_end, every[n_every / 2],
               every[n_every * 95 / 100], 100.0 * (double)over30 / (double)n_every);
        /* v3 had the far ones' last fix, minutes old, and the call with those in reach only */
        assert(spread > 7000 && near[n_near * 95 / 100] <= 3.5 && far[n_far / 2] > 3.5 && far[n_far / 2] < 60);
        assert(quick >= 15 && by_end >= quick);
    }

    /* Two riders given one invitation, so one slot: their packets meet every frame, nobody hears
     * either, and they cannot hear each other while both send. Listening in their slot one frame in
     * 32, at random, they hear each other within a minute or so, as a clash (T-join). */
    {
        static world_t x;
        const scenario_t five = {5, 2000, 600 * SEC, 0, 11, 1, GOOD, 0, false};
        world_init(&x, &five, 1792670400000ll);
        x.node[4].cfg.slot = 3;
        int64_t t = 0, found = -1;
        for (; t < five.duration_us && found < 0; t += STEP_US) {
            world_step(&x, t);
            if (x.node[3].rx_clash || x.node[4].rx_clash) found = t;
        }
        printf("sim: two riders in one slot are unseen by the others, and hear each other as a clash after %.0f s\n",
               (double)found / SEC);
        assert(found > 0 && found < 300 * SEC);
    }

    /* Two groups of three on one channel: each sees only its own riders, and drops the other's
     * packets by their tag. */
    static world_t w;
    const scenario_t two = {6, 2000, 300 * SEC, 0, 4, 2, GOOD, 0, false};
    world_init(&w, &two, 1792670400000ll);
    for (int64_t t = 0; t < two.duration_us; t += STEP_US) world_step(&w, t);
    unsigned long foreign = 0;
    for (int i = 0; i < 6; i++) {
        foreign += w.node[i].rx_foreign;
        assert(w.node[i].rx_forged == 0 && w.node[i].rx_bad == 0);
        for (int s = 0; s < NODE_SLOTS; s++) assert(w.node[i].riders[s].known == (s < 3 && s != w.slot_of[i]));
    }
    printf("sim: two groups of 3 on one channel: each knows only its own riders; %lu packets of the other "
           "group dropped by their tag\n", foreign);
    assert(foreign > 0);

    /* Two groups of ten, the same frame, on one channel: never within a group; between them, in the
     * frames whose phases fall within a packet of each other, about a third, and no rider worse off
     * than the rest. */
    const scenario_t neighbours = {20, 2000, 600 * SEC, 0, 8, 2, GOOD, 0, false};
    o = run(&neighbours, false);
    printf("sim: two groups of 10 on one channel: %.1f %% delivered, the worst rider %.1f %%, %d collisions, "
           "%d of them within a group; losses in bursts: fix age p95 %.1f s, the longest gap %.0f s, the longest "
           "a rider heard none of its group %.0f s\n", 100 * o.delivered, 100 * o.worst, o.collisions, o.within,
           o.age_p95, o.gap, o.gap_group);
    assert(o.within == 0 && o.delivered > 0.55 && o.delivered < 0.78 && o.worst > 0.5);
    return 0;
}
