#include "run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "port.h"
#include "sx1262.h"

#define TAG "convoy"

/* Until M5's invitation sets them: the group by hand, the slot from the chip's ID unless the build
 * or the console names one. */
#ifndef NODE_GROUP
#define NODE_GROUP 1
#endif
#ifndef NODE_INTERVAL_MS
#define NODE_INTERVAL_MS 2000
#endif
/* 14 dBm on both boards: 25 mW, inside every EU sub-band's limit. */
#ifndef NODE_POWER_LEVEL
#define NODE_POWER_LEVEL 3
#endif

#define VERIFY_US (30 * 1000000ll)
#define GNSS_LOG_US (60 * 1000000ll)

static node_t s_node;
static port_mutex_t s_lock;
static uint8_t s_power = NODE_POWER_LEVEL;
static volatile bool s_reapply;

/* Console lines start with a word a bench script can look for: TX, RX, RIDER, STATUS, GNSS. */
static void line(const char *text) { LOGI("line", "%s", text); }

static void gnss_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    nmea_t nmea;
    nmea_init(&nmea);
    port_gnss_open();
    int64_t logged = -GNSS_LOG_US;
    bool was_valid = false;
    for (;;) {
        port_wdt_feed(watch);
        const int c = port_gnss_getc(200);
        gnss_fix_t fix;
        if (c < 0 || !nmea_feed(&nmea, (char)c, &fix)) continue;
        const int64_t now = port_uptime_us();
        port_mutex_lock(s_lock);
        node_on_fix(&s_node, &fix, now);
        port_mutex_give(s_lock);
        if (fix.valid != was_valid || now - logged >= GNSS_LOG_US) {
            char text[96];
            snprintf(text, sizeof(text), "GNSS fix=%d time=%d sats=%u hdop=%u.%u bad=%lu", fix.valid,
                     fix.time_valid, fix.sats, fix.hdop10 / 10, fix.hdop10 % 10, (unsigned long)nmea.bad);
            line(text);
            was_valid = fix.valid;
            logged = now;
        }
    }
}

static void radio_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    radio_init();
    while (!radio_apply(&s_node.cfg.phy, s_power)) {
        LOGE(TAG, "the radio will not take its configuration; trying again");
        port_wdt_feed(watch);
        port_sleep_ms(5000);
    }
    int64_t next_verify = port_uptime_us() + VERIFY_US;
    char text[256];
    for (;;) {
        port_wdt_feed(watch);
        uint8_t buf[256];
        int rssi = 0, snr = 0;
        const size_t n = radio_receive(buf, sizeof(buf), 20, &rssi, &snr);
        int64_t now = port_uptime_us();
        if (n) {
            port_mutex_lock(s_lock);
            const bool taken = node_on_packet(&s_node, buf, n, rssi, snr, now);
            if (taken) node_format_rider(&s_node, buf[2], now, text, sizeof(text));
            port_mutex_give(s_lock);
            if (!taken) snprintf(text, sizeof(text), "RX dropped len=%u rssi=%d", (unsigned)n, rssi);
            line(text);
        }
        port_mutex_lock(s_lock);
        const size_t len = node_poll_tx(&s_node, now, buf, sizeof(buf));
        const uint32_t counter = s_node.counter - 1;
        port_mutex_give(s_lock);
        if (len) {
            const bool sent = radio_send(buf, len);
            snprintf(text, sizeof(text), "TX counter=%lu len=%u %s", (unsigned long)counter, (unsigned)len,
                     sent ? "ok" : "FAILED");
            line(text);
        }
        now = port_uptime_us();
        if (s_reapply || now >= next_verify) {
            if (s_reapply || !radio_verify()) {
                s_reapply = false;
                radio_apply(&s_node.cfg.phy, s_power);
            }
            next_verify = now + VERIFY_US;
        }
    }
}

/* ---- the console ---------------------------------------------------------------- */

static bool number(const char *s, long lo, long hi, long *out) {
    char *end;
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v < lo || v > hi) return false;
    *out = v;
    return true;
}

static void command(char *cmd) {
    char text[256];
    const int64_t now = port_uptime_us();
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = '\0';
    long v;
    port_mutex_lock(s_lock);
    if (strcmp(cmd, "status") == 0) {
        node_format_status(&s_node, now, text, sizeof(text));
    } else if (strcmp(cmd, "riders") == 0) {
        int known = 0;
        for (unsigned i = 0; i < NODE_SLOTS; i++) {
            if (!s_node.riders[i].known) continue;
            node_format_rider(&s_node, i, now, text, sizeof(text));
            line(text);
            known++;
        }
        snprintf(text, sizeof(text), "RIDERS %d", known);
    } else if (strcmp(cmd, "slot") == 0 && arg && number(arg, 0, NODE_SLOTS - 1, &v)) {
        s_node.cfg.slot = (uint8_t)v;
        snprintf(text, sizeof(text), "OK slot=%ld", v);
    } else if (strcmp(cmd, "group") == 0 && arg && number(arg, 0, 255, &v)) {
        s_node.cfg.group = (uint8_t)v;
        snprintf(text, sizeof(text), "OK group=%ld", v);
    } else if (strcmp(cmd, "interval") == 0 && arg && number(arg, 500, 60000, &v)) {
        s_node.cfg.interval_ms = (uint32_t)v;
        snprintf(text, sizeof(text), "OK interval=%ld", v);
    } else if (strcmp(cmd, "power") == 0 && arg && number(arg, 0, 3, &v)) {
        s_power = (uint8_t)v;
        s_reapply = true;
        snprintf(text, sizeof(text), "OK power=%ld dbm=%d", v, radio_tx_dbm((uint8_t)v));
    } else if (strcmp(cmd, "reboot") == 0) {
        port_mutex_give(s_lock);
        port_restart(0);
        return;
#ifdef PORT_BOOTLOADER
    } else if (strcmp(cmd, "bootloader") == 0) {
        /* the UF2 drive, for a new image without the reset button's double tap */
        port_mutex_give(s_lock);
        port_restart_to_bootloader();
        return;
#endif
    } else {
        snprintf(text, sizeof(text), "ERR commands: status, riders, slot 0-%d, group 0-255, "
                 "interval 500-60000, power 0-3 (0 strongest), reboot", NODE_SLOTS - 1);
    }
    port_mutex_give(s_lock);
    line(text);
}

static void console_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    port_console_open();
    char buf[64];
    size_t len = 0;
    for (;;) {
        port_wdt_feed(watch);
        const int c = port_console_getc(1000);
        if (c < 0) continue;
        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            if (len) command(buf);
            len = 0;
        } else if (len + 1 < sizeof(buf)) {
            buf[len++] = (char)c;
        }
    }
}

void run_start(const char *board) {
    uint8_t id[16];
    const size_t id_len = port_device_id(id, sizeof(id));
    uint32_t h = 2166136261u; /* FNV-1a of the chip's ID */
    for (size_t i = 0; i < id_len; i++) h = (h ^ id[i]) * 16777619u;
#ifdef NODE_SLOT
    const uint8_t slot = NODE_SLOT;
#else
    const uint8_t slot = (uint8_t)(h % NODE_SLOTS);
#endif
    const node_config_t cfg = {.group = NODE_GROUP, .slot = slot, .interval_ms = NODE_INTERVAL_MS,
                               .duty_permille = 100, .phy = PHY_V0};
    s_lock = port_mutex_create();
    node_init(&s_node, &cfg, port_random() ^ h, port_uptime_us());
    LOGI(TAG, "LoRa Convoy, protocol v%d, on %s after %s: group %u, slot %u%s, %u ms, %d dBm", PKT_VERSION,
         board, port_reset_cause(), cfg.group, cfg.slot,
#ifdef NODE_SLOT
         " (from the build)",
#else
         " (from the chip's ID; 'slot N' changes it)",
#endif
         (unsigned)cfg.interval_ms, radio_tx_dbm(s_power));
    port_task_create(gnss_task, "gnss", 4096, 4);
    port_task_create(radio_task, "radio", 6144, 5);
    port_task_create(console_task, "console", 4096, 3);
}
