#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int api_status(const api_t *api, int64_t now_us, char *buf, size_t cap) {
    char node[320];
    node_format_status(api->node, now_us, node, sizeof(node));
    return snprintf(buf, cap, "STATUS proto=%d board=%s reset=%s up=%lld %s power=%u dbm=%d", API_PROTO,
                    api->board, api->reset, (long long)(now_us / 1000000), node, api->power,
                    api->tx_dbm(api->power));
}

static bool number(const char *s, long lo, long hi, long *out) {
    if (!s) return false;
    char *end;
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
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

void api_command(api_t *api, char *line, int64_t now_us, api_reply_fn reply, void *ctx) {
    char out[400];
    uint8_t key[16];
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    node_config_t *cfg = &api->node->cfg;
    long v;
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
    } else if (strcasecmp(line, "key") == 0 && hex_key(arg, key)) {
        node_set_key(api->node, key);
        memset(key, 0, sizeof(key));
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK keyed group=%02x", api->node->group.tag);
    } else if (strcasecmp(line, "slot") == 0 && number(arg, 0, NODE_SLOTS - 1, &v)) {
        cfg->slot = (uint8_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK slot=%ld", v);
    } else if (strcasecmp(line, "interval") == 0 && number(arg, 500, 60000, &v)) {
        cfg->interval_ms = (uint32_t)v;
        api->setup_changed = true;
        snprintf(out, sizeof(out), "OK interval=%ld", v);
    } else if (strcasecmp(line, "power") == 0 && number(arg, 0, 3, &v)) {
        api->power = (uint8_t)v;
        api->power_changed = true;
        snprintf(out, sizeof(out), "OK power=%ld dbm=%d", v, api->tx_dbm((uint8_t)v));
    } else if (strcasecmp(line, "forget") == 0 && !arg) {
        api->forget = true;
        return; /* answered by whoever can forget: FORGOTTEN, or an ERR */
    } else if (strcasecmp(line, "reboot") == 0 && !arg) {
        api->restart = true;
        return;
    } else if (strcasecmp(line, "bootloader") == 0 && !arg) {
        api->bootloader = true;
        return;
    } else {
        snprintf(out, sizeof(out), "ERR commands: STATUS, RIDERS, KEY <32 hex>, SLOT 0-%d, INTERVAL 500-60000, "
                 "POWER 0-3 (0 strongest), FORGET, REBOOT, BOOTLOADER", NODE_SLOTS - 1);
    }
    reply(ctx, out);
}
