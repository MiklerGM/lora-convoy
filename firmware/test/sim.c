/*
 * Several node cores on simulated air (ARCHITECTURE, testing without hardware).
 *
 * The air, for now: everyone hears everyone; a packet is lost to a receiver that transmits during
 * any of it, to everyone when two transmissions overlap (no capture effect), and at random with a
 * set probability. Each rider rides a circle around Vaduz and gets a fix every second, all on one
 * perfect clock. Clock models come with slots by GPS time (M6).
 *
 *   sim                       the checks: three riders hear each other; twenty show plain ALOHA
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

typedef struct {
    int nodes;
    uint32_t interval_ms;
    int64_t duration_us;
    double loss;           /* random loss per packet and receiver */
    uint32_t seed;
    int groups;            /* riders split into this many groups, each with its own key; 0: no key */
} scenario_t;

typedef struct {
    double delivered;        /* of all packets sent, the share each other node took */
    double age_p50, age_p95; /* seconds: the age of the fix each node shows for each other, sampled
                                every second (07's headline numbers); never seen counts as forever */
    int collisions;
    int sent;
} outcome_t;

/* The simulated world: the nodes, the air between them, and what happened. */
typedef struct {
    scenario_t sc;
    node_t node[MAX_NODES];
    transmission_t air[MAX_AIR];
    int in_air;
    uint32_t rng;
    int64_t utc0_ms;
    int sent, collisions;
    long taken;
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

static void world_init(world_t *w, const scenario_t *sc, int64_t utc0_ms) {
    memset(w, 0, sizeof(*w));
    w->sc = *sc;
    w->rng = sc->seed ? sc->seed : 1;
    w->utc0_ms = utc0_ms;
    for (int i = 0; i < sc->nodes; i++) {
        /* group g has the key g, g, g ...; riders split into groups by halves, thirds ... */
        const int g = sc->groups ? i * sc->groups / sc->nodes : 0;
        node_config_t cfg = {.keyed = sc->groups > 0, .slot = (uint8_t)i, .interval_ms = sc->interval_ms,
                             .duty_permille = 100, .phy = PHY_V0};
        memset(cfg.key, g + 1, sizeof(cfg.key));
        node_init(&w->node[i], &cfg, splitmix(sc->seed * 1000u + (uint32_t)i), 0);
    }
}

static void world_step(world_t *w, int64_t t) {
    for (int i = 0; i < w->sc.nodes; i++) {
        /* the GNSS: a fix every second, each rider's on its own phase within it */
        if ((t + i * 37000) % SEC == 0) {
            gnss_fix_t f = ride(i, t, w->utc0_ms);
            node_on_fix(&w->node[i], &f, t);
        }
        uint8_t b[32];
        const size_t len = node_poll_tx(&w->node[i], t, b, sizeof(b));
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
        }
    }
    /* deliver what ended now */
    for (int k = 0; k < w->in_air; k++) {
        transmission_t *x = &w->air[k];
        if (x->done || x->end > t) continue;
        x->done = true;
        bool collided = false;
        for (int j = 0; j < w->in_air; j++) {
            if (j != k && w->air[j].start < x->end && w->air[j].end > x->start) collided = true;
        }
        w->collisions += collided;
        for (int i = 0; i < w->sc.nodes && !collided; i++) {
            if (i == x->from) continue;
            /* half duplex: a receiver sending during any of it hears none of it */
            bool busy = false;
            for (int j = 0; j < w->in_air; j++) {
                if (w->air[j].from == i && w->air[j].start < x->end && w->air[j].end > x->start) busy = true;
            }
            if (busy || (double)(rnd(&w->rng) % 1000000) / 1e6 < w->sc.loss) continue;
            if (node_on_packet(&w->node[i], x->bytes, x->len, -90, 20, t)) {
                w->taken++;
                if (w->on_taken) w->on_taken(w->ctx, i, x->bytes[2], t);
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

static outcome_t run(const scenario_t *sc, bool print) {
    static world_t w;
    world_init(&w, sc, 1792670400000ll);
    outcome_t out = {0};
    const size_t max_ages = (size_t)(sc->duration_us / SEC + 1) * (size_t)(sc->nodes * sc->nodes);
    double *ages = malloc(max_ages * sizeof(double));
    size_t n_ages = 0;
    assert(ages);
    for (int64_t t = 0; t < sc->duration_us; t += STEP_US) {
        world_step(&w, t);
        /* what each map shows, once a second, after a warm-up of ten intervals */
        if (t % SEC == SEC / 2 && t > 10 * (int64_t)sc->interval_ms * 1000) {
            for (int i = 0; i < sc->nodes; i++) {
                int64_t utc;
                if (!node_utc_ms(&w.node[i], t, &utc)) continue;
                for (int j = 0; j < sc->nodes; j++) {
                    if (j == i || n_ages >= max_ages) continue;
                    const rider_t *r = &w.node[i].riders[j];
                    ages[n_ages++] = r->known ? (double)(utc - r->fix_utc_ms) / 1000 : 1e9;
                }
            }
        }
    }
    /* The end of the run: what one node shows for the next. */
    char line[256];
    for (int i = 0; print && i < sc->nodes; i++) {
        node_format_rider(&w.node[i], (unsigned)((i + 1) % sc->nodes), sc->duration_us, line, sizeof(line));
        printf("node %2d sees %s\n", i, line);
    }
    qsort(ages, n_ages, sizeof(double), by_value);
    out.age_p50 = n_ages ? ages[n_ages / 2] : 1e9;
    out.age_p95 = n_ages ? ages[n_ages * 95 / 100] : 1e9;
    free(ages);
    out.sent = w.sent;
    out.collisions = w.collisions;
    out.delivered = w.sent ? (double)w.taken / ((double)w.sent * (sc->nodes - 1)) : 0;
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
    char out[450];
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
    const scenario_t sc = {n, 2000, 0, 0, (uint32_t)now.tv_sec, 0};
    world_init(&w, &sc, (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
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
                    api_command(&b->api, b->in, t, send_line, b);
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
            char line[400];
            api_status(&b->api, t, line, sizeof(line));
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
        const scenario_t sc = {atoi(argv[1]), (uint32_t)atoi(argv[2]), atoll(argv[3]) * SEC, 0, 42, 1};
        const outcome_t o = run(&sc, true);
        printf("%d riders every %u ms for %s s: %d sent, %d collided, %.1f %% delivered, "
               "fix age shown p50 %.1f s, p95 %.1f s\n", sc.nodes, sc.interval_ms, argv[3], o.sent,
               o.collisions, 100 * o.delivered, o.age_p50, o.age_p95);
        return 0;
    }

    /* Three riders, a packet every 2 s: each hears the others, with fixes a few seconds old. */
    const scenario_t three = {3, 2000, 600 * SEC, 0, 1, 1};
    outcome_t o = run(&three, false);
    printf("sim: 3 riders every 2 s: %.1f %% delivered, %d collisions, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.collisions, o.age_p50, o.age_p95);
    assert(o.delivered > 0.90 && o.age_p95 <= 5);

    /* With 10 % random loss on top, still everyone, a little older. */
    const scenario_t lossy = {3, 2000, 600 * SEC, 0.10, 2, 1};
    o = run(&lossy, false);
    printf("sim: 3 riders, 10 %% random loss: %.1f %% delivered, fix age p50 %.1f s, p95 %.1f s\n",
           100 * o.delivered, o.age_p50, o.age_p95);
    assert(o.delivered > 0.80 && o.delivered < 0.90 && o.age_p95 <= 7);

    /* Twenty riders every 2 s on random access: the channel carries G = 20 x 30.8 ms / 2 s = 0.31 of
     * its time, and pure ALOHA delivers about exp(-2G) = 54 %, less what the receivers' own sending
     * costs them. The reason slots by GPS time come next (M6). */
    const scenario_t twenty = {20, 2000, 600 * SEC, 0, 3, 1};
    o = run(&twenty, false);
    printf("sim: 20 riders every 2 s on random access: %.1f %% delivered (pure ALOHA predicts about 54 %%), "
           "fix age p50 %.1f s, p95 %.1f s\n", 100 * o.delivered, o.age_p50, o.age_p95);
    assert(o.delivered > 0.45 && o.delivered < 0.62);

    /* Two groups of three on one channel: each sees only its own riders, and drops the other's
     * packets by their tag; the air they share still costs both their collisions. */
    static world_t w;
    const scenario_t two = {6, 2000, 300 * SEC, 0, 4, 2};
    world_init(&w, &two, 1792670400000ll);
    for (int64_t t = 0; t < two.duration_us; t += STEP_US) world_step(&w, t);
    unsigned long foreign = 0;
    for (int i = 0; i < 6; i++) {
        foreign += w.node[i].rx_foreign;
        assert(w.node[i].rx_forged == 0 && w.node[i].rx_bad == 0);
        for (int j = 0; j < 6; j++) {
            if (j != i) assert(w.node[i].riders[j].known == (i / 3 == j / 3));
        }
    }
    printf("sim: two groups of 3 on one channel: each knows only its own riders; %lu packets of the other "
           "group dropped by their tag\n", foreign);
    assert(foreign > 0);
    return 0;
}
