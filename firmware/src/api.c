#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int api_status(const api_t *api, int64_t now_us, char *buf, size_t cap) {
    char node[API_LINE];   /* the node's part never takes more than a whole line */
    node_format_status(api->node, now_us, node, sizeof(node));
    /* at: when it was made, on our clock in ms, which a later GNSS time or a better reckoning
     * of it against the phone's leaves meaning the same */
    return snprintf(buf, cap, "STATUS proto=%d board=%s reset=%s at=%lld %s power=%u dbm=%d kept=%d",
                    API_PROTO, api->board, api->reset, (long long)(now_us / 1000), node, api->power,
                    api->tx_dbm(api->power), !api->save_failed);
}

static bool number(const char *s, long lo, long hi, long *out) {
    if (!s) return false;
    char *end;
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Slots as "3" or "3,4,17", a bit each; "-" for none. */
static bool slot_list(const char *s, uint32_t *mask) {
    if (!s) return false;
    *mask = 0;
    if (strcmp(s, "-") == 0) return true;
    for (;;) {
        char *end;
        const long v = strtol(s, &end, 10);
        if (end == s || v < 0 || v >= NODE_SLOTS) return false;
        *mask |= 1u << v;
        if (!*end) return true;
        if (*end != ',') return false;
        s = end + 1;
    }
}

/* 32 hex digits, either case, into 16 bytes. */
static bool hex_key(const char *s, uint8_t key[16]) {
    if (!s || strlen(s) != 32) return false;
    for (int i = 0; i < 32; i++) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        key[i / 2] = (uint8_t)(i % 2 ? key[i / 2] | v : v << 4);
    }
    return true;
}

/* The next word of *s, ended where it stands; NULL after the last. */
static char *word(char **s) {
    while (**s == ' ') (*s)++;
    if (!**s) return NULL;
    char *w = *s;
    while (**s && **s != ' ') (*s)++;
    if (**s) *(*s)++ = '\0';
    return w;
}

/* "<key> <slot> <slots> <interval>": a group's whole setup, checked whole before any of it is
 * taken, so a board is never left with one group's key and another's slot. */
static bool group_setup(char *s, uint8_t key[16], long *slot, long *slots, long *interval) {
    if (!s) return false;
    char *k = word(&s), *a = word(&s), *b = word(&s), *c = word(&s);
    return hex_key(k, key) && number(b, 1, NODE_SLOTS, slots) && number(a, 0, *slots - 1, slot) &&
           number(c, 500, 60000, interval) && !word(&s);
}

/* "<our clock, ms> <the phone's UTC then, ms> <its error, ms>": TIME's three, whole. */
static bool phone_time(const char *s, long long *at_ms, long long *utc_ms, long long *err_ms) {
    char extra;
    return s && sscanf(s, "%lld %lld %lld%c", at_ms, utc_ms, err_ms, &extra) == 3 && *at_ms >= 0 && *utc_ms > 0 &&
           *err_ms >= 0;
}

/* A moment of the phone's clock, in ms since it started: a phone up 25 days is past what a long
 * holds on our boards. */
static bool phone_ms(const char *s, long long *ms) {
    char extra;
    return s && sscanf(s, "%lld%c", ms, &extra) == 1 && *ms >= 0;
}

/* "SF kHz", into the mode we have with those two changed, if the radio can take it. */
static bool mode(const char *s, phy_t now, phy_t *out) {
    long sf, khz;
    char extra;
    if (!s || sscanf(s, "%ld %ld%c", &sf, &khz, &extra) != 2 || sf < 7 || sf > 12 || khz < 62 || khz > 500) return false;
    now.sf = (uint8_t)sf;
    now.bw_hz = khz == 62 ? 62500 : (uint32_t)khz * 1000;
    *out = now;
    return phy_supported(out);
}

void api_log(api_t *api, int64_t now_us, const char *text) {
    api_log_line_t *l = &api->log[api->log_next++ % API_LOG_LINES];
    l->at_ms = (uint32_t)(now_us / 1000);
    snprintf(l->text, sizeof(l->text), "%s", text);
}

static uint32_t log_oldest(const api_t *api) {
    return api->log_next > API_LOG_LINES ? api->log_next - API_LOG_LINES : 0;
}

bool api_log_peek(api_t *api, char *buf, size_t cap) {
    if (!api->log_on || api->log_sent >= api->log_next) return false;
    if (api->log_sent < log_oldest(api)) api->log_sent = log_oldest(api);
    const api_log_line_t *l = &api->log[api->log_sent % API_LOG_LINES];
    snprintf(buf, cap, "LOG n=%lu at=%lu %s", (unsigned long)api->log_sent, (unsigned long)l->at_ms, l->text);
    return true;
}

void api_log_sent(api_t *api) { api->log_sent++; }

/* "<boot, hex> <n>": where the phone's log of us ends. */
static bool log_from(const char *s, uint32_t boot, uint32_t next, uint32_t *from) {
    unsigned long b, n;
    char extra;
    if (!s || sscanf(s, "%lx %lu%c", &b, &n, &extra) != 2) return false;
    *from = b == boot && n <= next ? (uint32_t)n : 0;
    return true;
}

void api_command(api_t *api, char *line, int64_t came_us, int64_t now_us, api_reply_fn reply, void *ctx) {
    char out[API_LINE];
    phy_t phy;
    uint8_t key[16];
    long long at_ms, utc_ms, err_ms, t1_ms;
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    node_config_t *cfg = &api->node->cfg;
    long v;
    uint32_t mask, from;
    if (strcasecmp(line, "status") == 0) {
        api_status(api, now_us, out, sizeof(out));
    } else if (strcasecmp(line, "riders") == 0) {
        int known = 0;
        for (unsigned i = 0; i < NODE_SLOTS; i++) {
            if (!api->node->riders[i].known) continue;
            node_format_rider(api->node, i, now_us, out, sizeof(out));
            reply(ctx, out);
            known++;
        }
        snprintf(out, sizeof(out), "RIDERS %d", known);
    } else if (strcasecmp(line, "clock") == 0 && phone_ms(arg, &t1_ms)) {
        /* when the request came, and when the answer is made; and our GNSS time then, which the
         * phone tells its riders' ages by rather than its own calendar, or ? without one */
        char utc[24] = "?";
        int64_t utc_ms;
        if (node_utc_ms(api->node, now_us, &utc_ms)) snprintf(utc, sizeof(utc), "%lld", (long long)utc_ms);
        snprintf(out, sizeof(out), "CLOCK t1=%lld boot=%08lx rx=%lld tx=%lld utc=%s", t1_ms,
                 (unsigned long)api->node->boot, (long long)came_us, (long long)now_us, utc);
    } else if (strcasecmp(line, "time") == 0 && phone_time(arg, &at_ms, &utc_ms, &err_ms)) {
        node_set_phone_time(api->node, at_ms * 1000, utc_ms * 1000, err_ms * 1000);
        snprintf(out, sizeof(out), "OK time=phone err=%lld", (long long)err_ms);
    } else if (strcasecmp(line, "ledger") == 0 && !arg) {
        node_format_ledger(api->node, now_us, out, sizeof(out));
    } else if (strcasecmp(line, "key") == 0 && hex_key(arg, key)) {
        node_set_key(api->node, key);
        memset(key, 0, sizeof(key));
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK keyed group=%08lx", (unsigned long)api->node->group.id);
    } else if (strcasecmp(line, "group") == 0) {
        long slot, slots, interval;
        if (group_setup(arg, key, &slot, &slots, &interval)) {
            node_set_key(api->node, key);
            cfg->slot = (uint8_t)slot;
            cfg->slots = (uint8_t)slots;
            cfg->interval_ms = (uint32_t)interval;
            api->setup_changed = true;
            snprintf(out, sizeof(out), "OK group=%08lx slot=%ld slots=%ld interval=%ld", (unsigned long)api->node->group.id, slot,
                     slots, interval);
        } else {
            snprintf(out, sizeof(out), "ERR group: <32 hex digits> <slot below slots> <slots 1-%d> <interval 500-60000>",
                     NODE_SLOTS);
        }
        memset(key, 0, sizeof(key));
    } else if (strcasecmp(line, "mode") == 0 && mode(arg, cfg->phy, &phy)) {
        cfg->phy = phy;
        api->radio_changed = api->setup_changed = true;
        snprintf(out, sizeof(out), "OK mode=%u/%lu", phy.sf, (unsigned long)(phy.bw_hz / 1000));
    } else if (strcasecmp(line, "state") == 0 && number(arg, 0, STATES - 1, &v)) {
        node_set_state(api->node, (uint8_t)v);
        snprintf(out, sizeof(out), "OK state=%ld", v);
    } else if (strcasecmp(line, "help") == 0 && number(arg, 0, 1, &v)) {
        /* v3's word, for an app that has only that button: states 1 and 0 */
        node_set_state(api->node, v ? STATE_HELP : STATE_NORMAL);
        snprintf(out, sizeof(out), "OK help=%ld", v);
    } else if (strcasecmp(line, "beat") == 0 && number(arg, 0, 7, &v)) {
        /* the send rule's knobs, kept with the setup, as POWER is */
        cfg->beat = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK beat=%ld", v);
    } else if (strcasecmp(line, "move") == 0 && number(arg, 1, 1000, &v)) {
        cfg->move_m = (uint16_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK move=%ld", v);
    } else if (strcasecmp(line, "passengers") == 0 && number(arg, 0, PKT_PASSENGERS_MAX, &v)) {
        cfg->passengers = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK passengers=%ld", v);
    } else if (strcasecmp(line, "deaf") == 0 && slot_list(arg, &mask)) {
        cfg->deaf = mask;
        snprintf(out, sizeof(out), "OK deaf=%lx", (unsigned long)mask);
    } else if (strcasecmp(line, "slot") == 0 && number(arg, 0, NODE_SLOTS - 1, &v)) {
        cfg->slot = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK slot=%ld", v);
    } else if (strcasecmp(line, "slots") == 0 && number(arg, 1, NODE_SLOTS, &v)) {
        cfg->slots = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK slots=%ld", v);
    } else if (strcasecmp(line, "interval") == 0 && number(arg, 500, 60000, &v)) {
        cfg->interval_ms = (uint32_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK interval=%ld", v);
    } else if (strcasecmp(line, "power") == 0 && number(arg, 0, 3, &v)) {
        api->power = (uint8_t)v;
        api->radio_changed = api->setup_changed = true;
        snprintf(out, sizeof(out), "OK power=%ld dbm=%d", v, api->tx_dbm((uint8_t)v));
    } else if (strcasecmp(line, "log") == 0 && log_from(arg, api->node->boot, api->log_next, &from)) {
        api->log_sent = from < log_oldest(api) ? log_oldest(api) : from;
        api->log_on = true;
        snprintf(out, sizeof(out), "OK log boot=%08lx from=%lu next=%lu", (unsigned long)api->node->boot,
                 (unsigned long)api->log_sent, (unsigned long)api->log_next);
    } else if (strcasecmp(line, "forget") == 0 && !arg) {
        api->forget = true;
        return; /* answered by whoever can forget: FORGOTTEN, or an ERR */
    } else if (strcasecmp(line, "reboot") == 0 && !arg) {
        api->restart = true;
        return;
    } else if (strcasecmp(line, "bootloader") == 0 && !arg) {
        api->bootloader = true;
        return;
    } else if (strcasecmp(line, "tour") == 0 && number(arg, 0, 255, &v)) {
        api->tour_s = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK tour=%ld", v);
    } else if (strcasecmp(line, "zone") == 0 && number(arg, -720, 840, &v) && v % 15 == 0) {
        /* the phone's offset from UTC, for the screen's clock; kept, in quarter hours */
        api->zone_min = (int16_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK zone=%ld", v);
    } else if (strcasecmp(line, "gnss") == 0 && number(arg, 0, 4, &v)) {
        /* the GNSS off with the board running on the time it holds, for the bench (13, 2.7); or
         * restarted hot, warm or cold, a cold one forgetting the satellites' orbits */
        if (v <= 1) api->gnss_ask = (uint8_t)(v + 1);
        else api->gnss_start = (uint8_t)(v - 1);
        snprintf(out, sizeof(out), "OK gnss=%ld", v);
    } else if (strcasecmp(line, "systems") == 0 && number(arg, 1, 8, &v)) {
        /* which satellites the GNSS uses: its lag grows with their number (07, 2026-11-12) */
        api->gnss_systems = (uint8_t)v;
        snprintf(out, sizeof(out), "OK systems=%ld", v);
    } else if (strcasecmp(line, "gsv") == 0 && number(arg, 0, 1, &v)) {
        /* each satellite's C/N0 on the console, for the bench; NMEA time goes late with it */
        api->gnss_gsv = v != 0;
        snprintf(out, sizeof(out), "OK gsv=%ld", v);
    } else {
        snprintf(out, sizeof(out), "ERR commands: STATUS, RIDERS, CLOCK <t1>, TIME <ms> <utc ms> <err ms>, LEDGER, GROUP <32 hex> <slot> <slots> <ms>, KEY <32 hex>, STATE 0-15, BEAT 0-7, MOVE <m>, PASSENGERS 0-3, DEAF <slots>|-, SLOT 0-%d, SLOTS 1-%d, "
                 "INTERVAL 500-60000, POWER 0-3 (0 strongest), MODE <SF> <kHz>, TOUR 0-255, ZONE <min>, GNSS 0-4 (off, on, restart hot, warm, cold), SYSTEMS 1-8, GSV 0-1, LOG <boot> <n>, FORGET, REBOOT, BOOTLOADER", NODE_SLOTS - 1,
                 NODE_SLOTS);
    }
    reply(ctx, out);
}

bool api_tour(api_t *api, int64_t now_us) {
    static const struct {
        uint8_t sf;
        uint32_t bw_hz;
    } STOP[] = {{7, 250000}, {9, 250000}, {10, 125000}};
    int64_t utc, err;
    if (!api->tour_s || !node_utc_us(api->node, now_us, &utc, &err)) return false;
    const size_t i = (size_t)(utc / (api->tour_s * 1000000ll) % 3);
    phy_t *phy = &api->node->cfg.phy;
    if (phy->sf == STOP[i].sf && phy->bw_hz == STOP[i].bw_hz) return false;
    phy->sf = STOP[i].sf;
    phy->bw_hz = STOP[i].bw_hz;
    return true;
}
