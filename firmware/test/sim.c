/*
 * Several node cores on simulated air, in virtual time (ARCHITECTURE, testing without hardware).
 *
 * The air, for now: everyone hears everyone; a packet is lost to a receiver that transmits during
 * any of it, to everyone when two transmissions overlap (no capture effect), and at random with a
 * set probability. Each rider rides a circle and gets a fix every second, all on one perfect
 * clock. Clock models come with slots by GPS time (M6).
 *
 *   sim                    the checks: three riders hear each other; twenty show plain ALOHA
 *   sim N INTERVAL_MS S    N riders, every INTERVAL_MS, for S seconds: prints what each heard
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"

#define PI 3.14159265358979323846

#define SEC 1000000ll
#define STEP_US 1000
#define MAX_NODES NODE_SLOTS
#define MAX_AIR 256
/* Kept this long after it ends, longer than any packet lasts, so later ones still see it overlap. */
#define KEEP_US (2 * SEC)

typedef struct {
    int from;
    int64_t start, end;
    bool done;
    uint8_t bytes[PKT_POSITION_LEN];
    size_t len;
} transmission_t;

typedef struct {
    int nodes;
    uint32_t interval_ms;
    int64_t duration_us;
    double loss;           /* random loss per packet and receiver */
    uint32_t seed;
} scenario_t;

typedef struct {
    double delivered;      /* of all packets sent, the share each other node took */
    double age_p50, age_p95; /* seconds: the age of the fix each node shows for each other, sampled
                                every second (07's headline numbers); never seen counts as forever */
    int collisions;
    int sent;
} outcome_t;

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

static outcome_t run(const scenario_t *sc, bool print) {
    static node_t nodes[MAX_NODES];
    transmission_t air[MAX_AIR];
    int in_air = 0;
    uint32_t seed = sc->seed;
    const int64_t utc0 = 1792670400000ll;
    outcome_t out = {0};
    long taken = 0;
    const size_t max_ages = (size_t)(sc->duration_us / SEC + 1) * (size_t)(sc->nodes * sc->nodes);
    double *ages = malloc(max_ages * sizeof(double));
    size_t n_ages = 0;
    assert(ages);

    for (int i = 0; i < sc->nodes; i++) {
        node_config_t cfg = {.group = 7, .slot = (uint8_t)i, .interval_ms = sc->interval_ms,
                             .duty_permille = 100, .phy = PHY_V0};
        node_init(&nodes[i], &cfg, splitmix(sc->seed * 1000u + (uint32_t)i), 0);
    }
    for (int64_t t = 0; t < sc->duration_us; t += STEP_US) {
        for (int i = 0; i < sc->nodes; i++) {
            /* the GNSS: a fix every second, each rider's on its own phase within it */
            if ((t + i * 37000) % SEC == 0) {
                gnss_fix_t f = ride(i, t, utc0);
                node_on_fix(&nodes[i], &f, t);
            }
            uint8_t b[32];
            const size_t len = node_poll_tx(&nodes[i], t, b, sizeof(b));
            if (len) {
                assert(in_air < MAX_AIR);
                transmission_t *x = &air[in_air++];
                x->from = i;
                x->start = t;
                x->end = t + phy_airtime_us(&PHY_V0, len);
                x->done = false;
                memcpy(x->bytes, b, len);
                x->len = len;
                out.sent++;
            }
        }
        /* deliver what ended now */
        for (int k = 0; k < in_air; k++) {
            transmission_t *x = &air[k];
            if (x->done || x->end > t) continue;
            x->done = true;
            bool collided = false;
            for (int j = 0; j < in_air; j++) {
                if (j != k && air[j].start < x->end && air[j].end > x->start) collided = true;
            }
            out.collisions += collided;
            for (int i = 0; i < sc->nodes && !collided; i++) {
                if (i == x->from) continue;
                /* half duplex: a receiver sending during any of it hears none of it */
                bool busy = false;
                for (int j = 0; j < in_air; j++) {
                    if (air[j].from == i && air[j].start < x->end && air[j].end > x->start) busy = true;
                }
                if (busy || (double)(rnd(&seed) % 1000000) / 1e6 < sc->loss) continue;
                taken += node_on_packet(&nodes[i], x->bytes, x->len, -90, 20, t);
            }
        }
        /* what each map shows, once a second, after a warm-up of ten intervals */
        if (t % SEC == SEC / 2 && t > 10 * (int64_t)sc->interval_ms * 1000) {
            for (int i = 0; i < sc->nodes; i++) {
                int64_t utc;
                if (!node_utc_ms(&nodes[i], t, &utc)) continue;
                for (int j = 0; j < sc->nodes; j++) {
                    if (j == i || n_ages >= max_ages) continue;
                    const rider_t *r = &nodes[i].riders[j];
                    ages[n_ages++] = r->known ? (double)(utc - r->fix_utc_ms) / 1000 : 1e9;
                }
            }
        }
        for (int k = 0; k < in_air;) {
            if (air[k].done && air[k].end + KEEP_US < t) {
                air[k] = air[--in_air];
            } else {
                k++;
            }
        }
    }

    /* The end of the run: what one node shows for the next. */
    char line[256];
    for (int i = 0; print && i < sc->nodes; i++) {
        node_format_rider(&nodes[i], (unsigned)((i + 1) % sc->nodes), sc->duration_us, line, sizeof(line));
        printf("node %2d sees %s\n", i, line);
    }
    qsort(ages, n_ages, sizeof(double), by_value);
    out.age_p50 = n_ages ? ages[n_ages / 2] : 1e9;
    out.age_p95 = n_ages ? ages[n_ages * 95 / 100] : 1e9;
    free(ages);
    out.delivered = out.sent ? (double)taken / ((double)out.sent * (sc->nodes - 1)) : 0;
    return out;
}

int main(int argc, char **argv) {
    if (argc == 4) {
        const scenario_t sc = {atoi(argv[1]), (uint32_t)atoi(argv[2]), atoll(argv[3]) * SEC, 0, 42};
        const outcome_t o = run(&sc, true);
        printf("%d riders every %u ms for %s s: %d sent, %d collided, %.1f %% delivered, "
               "fix age shown p50 %.1f s, p95 %.1f s\n", sc.nodes, sc.interval_ms, argv[3], o.sent,
               o.collisions, 100 * o.delivered, o.age_p50, o.age_p95);
        return 0;
    }

    /* Three riders, a packet every 2 s: each hears the others, with fixes a few seconds old. */
    const scenario_t three = {3, 2000, 600 * SEC, 0, 1};
    outcome_t o = run(&three, false);
    printf("sim: 3 riders every 2 s: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.collisions, o.age_p50, o.age_p95);
    assert(o.delivered > 0.90 && o.age_p95 <= 5);

    /* With 10 % random loss on top, still everyone, a little older. */
    const scenario_t lossy = {3, 2000, 600 * SEC, 0.10, 2};
    o = run(&lossy, false);
    printf("sim: 3 riders, 10 %% random loss: %.1f %% delivered, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.age_p50, o.age_p95);
    assert(o.delivered > 0.80 && o.delivered < 0.90 && o.age_p95 <= 7);

    /* Twenty riders every 2 s on random access: the channel carries G = 20 x 25.7 ms / 2 s = 0.26 of
     * its time, and pure ALOHA delivers about exp(-2G) = 60 %, less what the receivers' own sending
     * costs them. The reason v1 schedules its slots (M6). */
    const scenario_t twenty = {20, 2000, 600 * SEC, 0, 3};
    o = run(&twenty, false);
    printf("sim: 20 riders every 2 s on random access: %.1f %% delivered (pure ALOHA predicts about 58 %%), "
           "fix age p50 %.1f s, p95 %.1f s\n", 100 * o.delivered, o.age_p50, o.age_p95);
    assert(o.delivered > 0.50 && o.delivered < 0.66);
    return 0;
}
