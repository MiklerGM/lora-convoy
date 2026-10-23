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
#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

typedef struct {
    int from;
    int64_t start, end;
    bool done;
    uint8_t bytes[PKT_POSITION_LEN];
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
    /* told of every packet a node takes, for a phone connected to it */
    void (*on_taken)(void *ctx, int node, unsigned slot, int64_t t);
    void *ctx;
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

static void world_init(world_t *w, const scenario_t *sc, int64_t utc0_ms) {
    memset(w, 0, sizeof(*w));
    w->sc = *sc;
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
                             .nmea_err_us = (int32_t)c->told_err_us};
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
        const bool dark = i >= w->sc.nodes - c->dark && t >= c->dark_from_us && t < c->dark_from_us + c->dark_us;
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
            node_on_fix(&w->node[i], &f, local_us(w, i, w->clock[i].fix_due));
            w->clock[i].fix_pending = false;
        }
        uint8_t b[32];
        const size_t len = node_poll_tx(&w->node[i], local_us(w, i, t), b, sizeof(b));
        if (len) {
            assert(w->in_air < MAX_AIR);
            transmission_t *x = &w->air[w->in_air++];
            x->from = i;
            x->start = t;
            x->end = t + phy_airtime_us(&w->node[i].cfg.phy, len);
            x->done = false;
            memcpy(x->bytes, b, len);
            x->len = len;
            w->sent++;
            w->sent_by[i]++;
            w->expected += w->size_of[i] - 1;
        }
    }
    /* deliver what ended now */
    for (int k = 0; k < w->in_air; k++) {
        transmission_t *x = &w->air[k];
        if (x->done || x->end > t) continue;
        x->done = true;
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
            if (i == x->from || !reaches(w, x->from, i)) continue;
            /* half duplex: a receiver sending during any of it hears none of it; and one that also
             * hears another packet over it hears neither */
            bool lost = false;
            for (int j = 0; j < w->in_air && !lost; j++) {
                const transmission_t *o = &w->air[j];
                if (j == k || o->start >= x->end || o->end <= x->start) continue;
                lost = o->from == i || w->sc.range_m <= 0 ||
                       distance_m(w, o->from, i) < 1.5 * w->sc.range_m;
            }
            if (lost || (double)(rnd(&w->rng) % 1000000) / 1e6 < w->sc.loss) continue;
            if (node_on_packet(&w->node[i], x->bytes, x->len, -90, 20, local_us(w, i, t))) {
                w->taken++;
                w->taken_from[x->from]++;
                if (w->on_taken) w->on_taken(w->ctx, i, x->bytes[2], local_us(w, i, t));
            }
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
                    ages[n_ages++] = r->known ? (double)(utc - r->fix_utc_ms) / 1000 : 1e9;
                }
            }
        }
    }
    /* The end of the run: what one node shows for the next. */
    char line[256];
    for (int i = 0; print && i < sc->nodes; i++) {
        node_format_rider(&s_run.node[i], (unsigned)((s_run.slot_of[i] + 1) % s_run.size_of[i]),
                          local_us(&s_run, i, sc->duration_us), line, sizeof(line));
        printf("node %2d sees %s\n", i, line);
    }
    qsort(ages, n_ages, sizeof(double), by_value);
    out.age_p50 = n_ages ? ages[n_ages / 2] : 1e9;
    out.age_p95 = n_ages ? ages[n_ages * 95 / 100] : 1e9;
    free(ages);
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

static void on_taken(void *ctx, int node, unsigned slot, int64_t t) {
    world_t *w = ctx;
    board_t *b = &s_boards[node];
    if (b->client < 0) return;
    char line[256];
    node_format_rider(&w->node[node], slot, t, line, sizeof(line));
    send_line(b, line);
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
                if (b->in[0]) {
                    api_command(&b->api, b->in, local_us(&w, i, t), send_line, b);
                    if (b->api.forget) send_line(b, "ERR FORGET is for Bluetooth");
                    b->api.forget = b->api.restart = b->api.bootloader = b->api.power_changed = false;
                }
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
    printf("sim: 3 riders every 2 s: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.collisions, o.age_p50, o.age_p95);
    assert(o.delivered > 0.99 && o.collisions == 0 && o.age_p95 <= 3.5);

    /* With 10 % random loss on top, still everyone, a little older. */
    const scenario_t lossy = {3, 2000, 600 * SEC, 0.10, 2, 1, GOOD, 0, false};
    o = run(&lossy, false);
    printf("sim: 3 riders, 10 %% random loss: %.1f %% delivered, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.age_p50, o.age_p95);
    assert(o.delivered > 0.85 && o.delivered < 0.95 && o.age_p95 <= 5.5);

    /* Twenty riders every 2 s: random access lost 45 % of them to collisions (M5); slots lose none. */
    const scenario_t twenty = {20, 2000, 600 * SEC, 0, 3, 1, GOOD, 0, false};
    o = run(&twenty, false);
    printf("sim: 20 riders every 2 s in slots: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s "
           "(random access: 55 %%, p95 8.5 s)\n", 100 * o.delivered, o.collisions, o.age_p50, o.age_p95);
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

    /* A tunnel: three riders lose their GNSS for 15 minutes. Each holds its slot while its time is
     * sure enough, about 10 minutes at 50 ppm against 31 ms, then goes quiet, and comes back with
     * the fix. Nobody collides. */
    const clocks_t tunnel = {.drift_ppm = 20, .lag_us = 100000, .told_err_us = 100000, .dark = 3,
                             .dark_from_us = 60 * SEC, .dark_us = 900 * SEC};
    const scenario_t dark = {20, 2000, 1200 * SEC, 0, 6, 1, tunnel, 0, false};
    o = run(&dark, false);
    unsigned long quiet = 0, after = 0;
    for (int i = 17; i < 20; i++) {
        quiet += s_run.node[i].tx_unsure;
        after += s_run.sent_by[i];
    }
    printf("sim: 3 of 20 riders 15 min without GNSS: they held their slots about %lu s, then passed over "
           "%lu slots until the fix came back; %d collisions\n",
           (unsigned long)((900 - quiet / 3 * 2)), quiet, o.collisions);
    assert(o.collisions == 0 && quiet > 3 * 100 && quiet < 3 * 200 && after > 3 * 400);

    /* An L1 told nothing of its NMEA's lag, and sure of it, sends 100 ms late, into the next
     * rider's slot, every frame: why E0 measures the lag before a ride. Told it is unsure, as boards
     * are by default (100 ms), it keeps quiet in a group this size instead. */
    const clocks_t careless = {.nmea_only = 1, .lag_us = 100000, .jitter_us = 5000, .told_err_us = 10000};
    const scenario_t wrong = {20, 2000, 600 * SEC, 0, 7, 1, careless, 0, false};
    o = run(&wrong, false);
    const int wrong_within = o.within;
    const clocks_t honest = {.nmea_only = 1, .lag_us = 100000, .jitter_us = 5000, .told_err_us = 100000};
    const scenario_t unsure = {20, 2000, 600 * SEC, 0, 7, 1, honest, 0, false};
    o = run(&unsure, false);
    printf("sim: an L1 told nothing of its NMEA's lag, and sure of it, collides with the next rider in every "
           "frame (%d packets); told it is unsure, it keeps quiet (%d silent, %d collisions)\n", wrong_within,
           o.silent, o.within);
    assert(wrong_within > 500 && o.silent == 1 && o.within == 0);

    /* "Need help", answered (01, help delivery): each of twenty riders calls in turn, with 10 % of
     * packets lost; how long until one of the others answers, and until all of them have. */
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
            int64_t answered = 0;
            for (; __builtin_popcount(h.node[k].help_heard_by) < 19; t += STEP_US) {
                world_step(&h, t);
                if (h.node[k].help_heard_by && !answered) answered = t;
                assert(t - asked < 60 * SEC);
            }
            first[k] = (double)(answered - asked) / SEC;
            all[k] = (double)(t - asked) / SEC;
            node_set_help(&h.node[k], false);
            /* the next call at any moment of a frame */
            for (const int64_t end = t + 4 * SEC + rnd(&h.rng) % 2000 * 1000; t < end; t += STEP_US) world_step(&h, t);
        }
        qsort(first, 20, sizeof(double), by_value);
        qsort(all, 20, sizeof(double), by_value);
        printf("sim: a call for help among 20 riders, 10 %% of packets lost: first answered within %.1f s, by "
               "everyone within %.1f s (the median; at worst %.1f and %.1f s)\n", first[10], all[10], first[19],
               all[19]);
        assert(first[19] < 5 && all[19] < 20);
    }

    /* A column of twenty on one road, spreading out to some 9 km in 20 minutes, its radios in reach
     * for sure within 1 km, as in forest (06: out of range means stale, not lost). Rider 6 stops
     * after ten minutes for five, and calls for help: those in reach answer at once, those behind as
     * they come by, and those ahead not at all. */
    {
        static world_t c;
        const scenario_t road = {20, 2000, 1200 * SEC, 0, 10, 1, GOOD, 1000, true};
        world_init(&c, &road, 1792670400000ll);
        static double near[20 * 19 * 1200], far[20 * 19 * 1200];
        size_t n_near = 0, n_far = 0;
        int quick = 0, by_end = 0;
        for (int64_t t = 0; t < road.duration_us; t += STEP_US) {
            world_step(&c, t);
            if (t == STOP_FROM) node_set_help(&c.node[STOPPER], true);
            if (t == STOP_FROM + 10 * SEC) quick = __builtin_popcount(c.node[STOPPER].help_heard_by);
            if (t == STOP_FROM + STOP_FOR) {
                by_end = __builtin_popcount(c.node[STOPPER].help_heard_by);
                node_set_help(&c.node[STOPPER], false);
            }
            if (t % SEC != SEC / 2 || t < 20 * SEC) continue;
            for (int i = 0; i < 20; i++) {
                int64_t utc;
                if (!node_utc_ms(&c.node[i], local_us(&c, i, t), &utc)) continue;
                for (int j = 0; j < 20; j++) {
                    if (j == i) continue;
                    const rider_t *r = &c.node[i].riders[c.slot_of[j]];
                    const double age = r->known ? (double)(utc - r->fix_utc_ms) / 1000 : 1e9, d = distance_m(&c, i, j);
                    if (d <= road.range_m) near[n_near++] = age;
                    if (d >= 1.5 * road.range_m) far[n_far++] = age;
                }
            }
        }
        double spread = 0;
        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 20; j++) spread = fmax(spread, distance_m(&c, i, j));
        }
        qsort(near, n_near, sizeof(double), by_value);
        qsort(far, n_far, sizeof(double), by_value);
        printf("sim: a column of 20 spreading to %.1f km, radios reaching 1 km: in reach, fixes %.1f s old at the "
               "95th percentile; out of it, the last fix stays, %.0f s old at the median. A call for help from a "
               "stopped rider: %d answer within 10 s, %d of 19 before it rides on\n", spread / 1000,
               near[n_near * 95 / 100], far[n_far / 2], quick, by_end);
        assert(spread > 7000 && near[n_near * 95 / 100] <= 3.5 && far[n_far / 2] > 30 && far[n_far / 2] < 1e9);
        assert(quick >= 8 && by_end > quick && by_end < 19);
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
           "%d of them within a group\n", 100 * o.delivered, 100 * o.worst, o.collisions, o.within);
    assert(o.within == 0 && o.delivered > 0.55 && o.delivered < 0.78 && o.worst > 0.5);
    return 0;
}
