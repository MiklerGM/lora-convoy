#include "run.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "ble_nus.h"
#include "node.h"
#include "port.h"
#include "sx1262.h"

#define TAG "convoy"

/* Until the app gives a setup from an invitation (KEY, SLOT): no key, so silent, and a slot from the
 * chip's ID unless the build names one. */
#ifndef NODE_INTERVAL_MS
#define NODE_INTERVAL_MS 2000
#endif
/* 14 dBm on both boards: 25 mW, inside every EU sub-band's limit. */
#ifndef NODE_POWER_LEVEL
#define NODE_POWER_LEVEL 3
#endif
/* The L76K's NMEA lag behind its second, for boards without PPS: unmeasured, so taken as nothing,
 * give or take 100 ms. A Heltec with its kit measures it (lag= in the status); E0 sets both. */
#ifndef NODE_NMEA_LAG_US
#define NODE_NMEA_LAG_US 0
#endif
#ifndef NODE_NMEA_ERR_US
#define NODE_NMEA_ERR_US 100000
#endif

#define VERIFY_US (30 * 1000000ll)
#define GNSS_LOG_US (60 * 1000000ll)
#define STATUS_PUSH_MS 10000

/* What a board keeps over a restart, so it can ride as a beacon with no phone: the setup the app
 * gave it from the invitation. */
typedef struct {
    uint8_t version;      /* SETUP_VERSION; anything else is ignored */
    uint8_t keyed;
    uint8_t slot;
    uint8_t slots;
    uint8_t key[16];
    uint32_t interval_ms;
    uint8_t sf;           /* the radio mode, on v0's frequency */
    uint8_t reserved;
    uint16_t bw_khz;      /* 62 for 62.5 */
} setup_t;
#define SETUP_VERSION 3

static node_t s_node;
static api_t s_api;
static port_mutex_t s_lock;
static atomic_bool s_reapply = ATOMIC_VAR_INIT(false);
static atomic_bool s_restart_after_bond_clear = ATOMIC_VAR_INIT(false);
static const char *s_reset = "other";

/* Console lines start with a word a bench script can look for: TX, RX, RIDER, STATUS, GNSS. */
static void console_line(const char *text) { LOGI("line", "%s", text); }

/* ---- why the board last started ------------------------------------------------ */

const char *run_reset_name(void) {
    const uint32_t note = port_restart_note();
    const char *cause = port_reset_cause();
    if (strcmp(cause, "sw") == 0) {
        if (note == (RESTART_NOTE_MAGIC | RESTART_FOR_BLE)) return "ble_stuck";
        if (note == (RESTART_NOTE_MAGIC | RESTART_FOR_BOND)) return "bond_reset";
    }
    return cause;
}

void run_erase_when_asked(const char *why, int watch, void (*erase)(void)) {
    int held_ms = 0, quiet_ms = 0;
    while (held_ms < PAIR_HOLD_MS) {
        port_wdt_feed(watch);
        if (quiet_ms == 0) {
            LOGE(TAG, "storage will not open (%s): bluetooth stays off until the button is held for %d s, "
                 "which erases it and forgets the paired phone", why, PAIR_HOLD_MS / 1000);
        }
        quiet_ms = (quiet_ms + 100) % 10000;
        held_ms = port_button_down() ? held_ms + 100 : 0;
        port_sleep_ms(100);
    }
    LOGW(TAG, "button held - erasing storage");
    erase();
    /* Released before the button task starts, which would read the same hold as a FORGET. */
    while (port_button_down()) {
        port_wdt_feed(watch);
        port_sleep_ms(100);
    }
}

/* ---- the phone API, from Bluetooth and the console ------------------------------- */

/* Replies are gathered under the node's lock and sent after it, so a slow phone never holds the
 * radio task up. One command at a time per transport: the BLE host and the console task each run
 * their own. */
#define REPLY_LINES (NODE_SLOTS + 2)
typedef struct {
    char line[REPLY_LINES][API_LINE];
    int n;
} replies_t;

static void gather(void *ctx, const char *line) {
    replies_t *r = ctx;
    if (r->n < REPLY_LINES) snprintf(r->line[r->n++], sizeof(r->line[0]), "%s", line);
}

/* Runs one command and carries out what api_command leaves to its caller. True if it asked to
 * forget the paired phone, which only the transport knows how to answer. */
static bool command(char *line, replies_t *out) {
    out->n = 0;
    port_mutex_lock(s_lock);
    api_command(&s_api, line, port_uptime_us(), gather, out);
    const bool forget = s_api.forget, restart = s_api.restart, bootloader = s_api.bootloader;
    if (s_api.radio_changed) atomic_store(&s_reapply, true);
    setup_t setup = {.version = SETUP_VERSION, .keyed = s_node.cfg.keyed, .slot = s_node.cfg.slot,
                     .slots = s_node.cfg.slots, .interval_ms = s_node.cfg.interval_ms, .sf = s_node.cfg.phy.sf,
                     .bw_khz = (uint16_t)(s_node.cfg.phy.bw_hz / 1000)};
    memcpy(setup.key, s_node.cfg.key, sizeof(setup.key));
    const bool save = s_api.setup_changed;
    s_api.forget = s_api.restart = s_api.bootloader = s_api.radio_changed = s_api.setup_changed = false;
    port_mutex_give(s_lock);
    if (save) {
        const bool kept = port_setup_save(&setup, sizeof(setup));
        port_mutex_lock(s_lock);
        s_api.save_failed = !kept;
        port_mutex_give(s_lock);
        if (!kept) gather(out, "ERR the setup could not be kept");
    }
    memset(&setup, 0, sizeof(setup));
    if (restart) port_restart(0);
#ifdef PORT_BOOTLOADER
    /* the UF2 drive, for a new image without the reset button's double tap */
    if (bootloader) port_restart_to_bootloader();
#else
    if (bootloader) gather(out, "ERR this board has no UF2 drive");
#endif
    return forget;
}

static void on_ble_line(const char *line, size_t len) {
    static replies_t replies;
    char buf[128];
    if (memchr(line, '\0', len) || len >= sizeof(buf)) {
        ble_nus_send_line("ERR malformed command");
        return;
    }
    memcpy(buf, line, len + 1);
    if (command(buf, &replies)) {
        /* Firmware clears first, then FORGOTTEN tells the app it may drop its own bond too. */
        ble_nus_forget_bonds();
        return;
    }
    for (int i = 0; i < replies.n; i++) ble_nus_send_line(replies.line[i]);
}

/* Nothing the phone asked for is queued, so a session that ends leaves nothing to fence. */
static void on_ble_revoke(void) {}

static void on_ble_bonds_cleared(void) { atomic_store(&s_restart_after_bond_clear, true); }

static void console_task(void *arg) {
    (void)arg;
    static replies_t replies;
    const int watch = port_wdt_watch();
    port_console_open();
    char buf[128];
    size_t len = 0;
    for (;;) {
        port_wdt_feed(watch);
        const int c = port_console_getc(1000);
        if (c < 0) continue;
        if (c != '\r' && c != '\n') {
            if (len + 1 < sizeof(buf)) buf[len++] = (char)c;
            continue;
        }
        buf[len] = '\0';
        if (len && command(buf, &replies)) {
            /* the console is a hand on the board, as the button is */
            gather(&replies, ble_nus_request_forget_bonds() ? "OK forgetting the paired phone"
                                                            : "ERR could not forget the paired phone");
        }
        for (int i = 0; i < replies.n; i++) console_line(replies.line[i]);
        replies.n = 0;
        len = 0;
    }
}

/* ---- the GNSS --------------------------------------------------------------------- */

static void gnss_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    nmea_t nmea;
    nmea_init(&nmea);
    port_gnss_open();
    int64_t logged = -GNSS_LOG_US, pps = 0;
    bool was_valid = false;
    for (;;) {
        port_wdt_feed(watch);
        const int c = port_gnss_getc(200);
        gnss_fix_t fix;
        if (c < 0 || !nmea_feed(&nmea, (char)c, &fix)) continue;
        /* ponytail: the fix's time is when its last sentence was parsed, after the UART's buffering;
         * a timestamp from the receive interrupt is finer, if E0 finds NMEA time too rough */
        const int64_t now = port_uptime_us();
        const int64_t edge = port_pps_us();
        port_mutex_lock(s_lock);
        /* the edge before the fix that names its second */
        if (edge != pps) node_on_pps(&s_node, pps = edge);
        node_on_fix(&s_node, &fix, now);
        port_mutex_give(s_lock);
        if (fix.valid != was_valid || now - logged >= GNSS_LOG_US) {
            char text[96];
            snprintf(text, sizeof(text), "GNSS fix=%d time=%d sats=%u hdop=%u.%u bad=%lu", fix.valid,
                     fix.time_valid, fix.sats, fix.hdop10 / 10, fix.hdop10 % 10, (unsigned long)nmea.bad);
            console_line(text);
            was_valid = fix.valid;
            logged = now;
        }
    }
}

/* ---- the radio -------------------------------------------------------------------- */

static void radio_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    radio_init();
    while (!radio_apply(&s_node.cfg.phy, s_api.power)) {
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
        /* Listening until shortly before our slot, at most 20 ms at a time. */
        port_mutex_lock(s_lock);
        int64_t due = node_next_tx_us(&s_node, port_uptime_us());
        port_mutex_give(s_lock);
        int64_t wait_ms = (due - port_uptime_us()) / 1000 - 1;
        const size_t n = radio_receive(buf, sizeof(buf), wait_ms < 0 ? 0 : wait_ms > 20 ? 20 : (uint32_t)wait_ms,
                                       &rssi, &snr);
        int64_t now = port_uptime_us();
        if (n) {
            port_mutex_lock(s_lock);
            const bool taken = node_on_packet(&s_node, buf, n, rssi, snr, now);
            if (taken) node_format_rider(&s_node, buf[2], now, text, sizeof(text));
            port_mutex_give(s_lock);
            if (taken) {
                if (ble_nus_connected()) ble_nus_send_line(text);
            } else {
                snprintf(text, sizeof(text), "RX dropped len=%u rssi=%d", (unsigned)n, rssi);
            }
            console_line(text);
        }
        /* The last stretch to our slot spun out, so the packet starts within a tick of its moment. */
        port_mutex_lock(s_lock);
        due = node_next_tx_us(&s_node, port_uptime_us());
        port_mutex_give(s_lock);
        while (due - port_uptime_us() > 0 && due - port_uptime_us() < 2000) {
        }
        now = port_uptime_us();
        port_mutex_lock(s_lock);
        const size_t len = node_poll_tx(&s_node, now, buf, sizeof(buf));
        const uint32_t counter = s_node.counter - 1;
        port_mutex_give(s_lock);
        if (len) {
            const bool sent = radio_send(buf, len);
            snprintf(text, sizeof(text), "TX counter=%lu len=%u %s", (unsigned long)counter, (unsigned)len,
                     sent ? "ok" : "FAILED");
            console_line(text);
        }
        now = port_uptime_us();
        const bool reapply = atomic_exchange(&s_reapply, false);
        if (reapply || now >= next_verify) {
            /* a new power level, or a chip that no longer holds our configuration */
            if (reapply || !radio_verify()) radio_apply(&s_node.cfg.phy, s_api.power);
            next_verify = now + VERIFY_US;
        }
    }
}

/* ---- the button, Bluetooth's health, and the status the phone gets unasked --------- */

static void service_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    int held_ms = 0, tick_ms = 0, status_ms = 0;
    bool fired = false;
    char text[API_LINE];
    for (;;) {
        port_wdt_feed(watch);
        if ((tick_ms += 100) >= 1000) {
            tick_ms = 0;
            if (ble_nus_supervise()) {
                LOGE(TAG, "bluetooth stuck - restarting");
                port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BLE);
            }
        }
        if ((status_ms += 100) >= STATUS_PUSH_MS) {
            status_ms = 0;
            if (ble_nus_connected()) {
                port_mutex_lock(s_lock);
                api_status(&s_api, port_uptime_us(), text, sizeof(text));
                port_mutex_give(s_lock);
                ble_nus_send_line(text);
            }
        }
        const bool down = port_button_down();
        if (down) {
            held_ms += 100;
            if (held_ms >= PAIR_HOLD_MS && !fired) {
                fired = true; /* once per press */
                LOGW(TAG, "button held - forgetting the paired phone");
                if (!ble_nus_request_forget_bonds()) LOGE(TAG, "the reset could not be scheduled");
            }
        } else {
            held_ms = 0;
            fired = false;
        }
        if (!down && atomic_load(&s_restart_after_bond_clear)) {
            /* The restart also clears the controller's resolving list. FORGOTTEN gets time to
             * reach the phone first, and a held button never restarts the board. */
            port_sleep_ms(4000);
            if (!port_button_down()) {
                LOGW(TAG, "bonds cleared - restarting Bluetooth");
                port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BOND);
            }
        }
        port_sleep_ms(100);
    }
}

/* ---- start ----------------------------------------------------------------------------- */

void run_start(const char *board, const char *reset) {
    s_reset = reset;
    uint8_t id[16];
    const size_t id_len = port_device_id(id, sizeof(id));
    uint32_t h = 2166136261u; /* FNV-1a of the chip's ID */
    for (size_t i = 0; i < id_len; i++) h = (h ^ id[i]) * 16777619u;
#ifdef NODE_SLOT
    const uint8_t slot = NODE_SLOT;
#else
    const uint8_t slot = (uint8_t)(h % NODE_SLOTS);
#endif
    node_config_t cfg = {.slot = slot, .slots = NODE_SLOTS, .interval_ms = NODE_INTERVAL_MS, .duty_permille = 100,
                         .phy = PHY_V0, .nmea_lag_us = NODE_NMEA_LAG_US, .nmea_err_us = NODE_NMEA_ERR_US,
                         .listen_one_in = 32};
    setup_t setup;
    const bool kept = port_setup_load(&setup, sizeof(setup)) && setup.version == SETUP_VERSION &&
                      setup.slot < NODE_SLOTS && setup.slots >= 1 && setup.slots <= NODE_SLOTS &&
                      setup.interval_ms >= 500 && setup.interval_ms <= 60000;
    if (kept) {
        cfg.keyed = setup.keyed;
        memcpy(cfg.key, setup.key, sizeof(cfg.key));
        cfg.slot = setup.slot;
        cfg.slots = setup.slots;
        cfg.interval_ms = setup.interval_ms;
        phy_t phy = cfg.phy;
        phy.sf = setup.sf;
        phy.bw_hz = setup.bw_khz == 62 ? 62500 : (uint32_t)setup.bw_khz * 1000;
        if (phy_supported(&phy)) cfg.phy = phy;
    }
    memset(&setup, 0, sizeof(setup));
    s_lock = port_mutex_create();
    node_init(&s_node, &cfg, port_random() ^ h);
    s_api = (api_t){.node = &s_node, .board = board, .reset = s_reset, .power = NODE_POWER_LEVEL,
                    .tx_dbm = radio_tx_dbm};
    LOGI(TAG, "LoRa Convoy, protocol v%d, phone API %d, on %s after %s: %s, slot %u of %u%s, every %u ms, %d dBm",
         PKT_VERSION, API_PROTO, board, s_reset,
         cfg.keyed ? "keyed" : "no key yet: silent until the app or KEY gives one", cfg.slot, cfg.slots,
         kept ? " (kept)" : " (from the chip's ID; SLOT N changes it)", (unsigned)cfg.interval_ms,
         radio_tx_dbm(s_api.power));
    ble_nus_init(on_ble_line, on_ble_revoke, on_ble_bonds_cleared);
    port_task_create(gnss_task, "gnss", 4096, 4);
    port_task_create(radio_task, "radio", 6144, 5);
    port_task_create(console_task, "console", 4096, 3);
    port_task_create(service_task, "service", 4096, 3);
}
