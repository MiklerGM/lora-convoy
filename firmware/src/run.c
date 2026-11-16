#include "run.h"

#include <stdatomic.h>
#include <stddef.h>
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
/* 24 dBm on the Heltec, 20 on boards with the bare chip: under the 27 dBm ERP of the sub-band we
 * use (PROTOCOL), with room for an antenna's gain. A desk wants POWER 3, and it is not kept. */
/* The crystal's drift bound, in ppm (node.h); 0 is the core's 50. */
#ifndef NODE_DRIFT_PPM
#define NODE_DRIFT_PPM 0
#endif
#ifndef NODE_POWER_LEVEL
#define NODE_POWER_LEVEL 1
#endif
/* The L76K's NMEA lag behind its second, for boards without PPS: taken as nothing, give or take
 * 100 ms, unless the board's build names what was measured on it (zephyr/src/board.h). A Heltec
 * with its kit measures it (lag= in the status). */
#ifndef NODE_NMEA_LAG_US
#define NODE_NMEA_LAG_US 0
#endif
#ifndef NODE_NMEA_ERR_US
#define NODE_NMEA_ERR_US 100000
#endif
#ifndef NODE_NMEA_LAG_SAT_US
#define NODE_NMEA_LAG_SAT_US 0
#endif
/* From handing the radio a packet to its RF beginning: each board's own, measured (txlead=). */
#ifndef NODE_TX_LEAD_US
#define NODE_TX_LEAD_US 0
#endif

#define VERIFY_US (30 * 1000000ll)
#define GNSS_LOG_US (60 * 1000000ll)
#define GNSS_TELL_US (10 * 1000000ll)
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
    uint8_t tour_s;       /* TOUR; 0 in every setup kept before there was one */
    uint16_t bw_khz;      /* 62 for 62.5 */
    /* from SETUP_VERSION 4: what the app's board screen sets, kept so a board with no phone rides
     * as it was set */
    uint8_t power;        /* POWER, 0 strongest */
    uint8_t beat;         /* BEAT */
    uint8_t passengers;   /* PASSENGERS */
    uint8_t spare;
    uint16_t move_m;      /* MOVE */
} setup_t;
#define SETUP_VERSION 4
/* A setup kept by a board before version 4 is its first part: taken, with the rest as built. */
#define SETUP_V3_LEN offsetof(setup_t, power)

static node_t s_node;
static api_t s_api;
static port_mutex_t s_lock;
/* One command at a time, whichever transport it came on, from its taking to its saving: a save
 * then always follows the setup it took, never an older one over a newer. The status pushed to the
 * phone is made and sent under it too, so one made before a GROUP never reaches the phone after the
 * GROUP's answer, to be taken for the board's group. Apart from s_lock, which stays as short as the
 * radio needs. */
static port_mutex_t s_command_lock;
static atomic_bool s_reapply = ATOMIC_VAR_INIT(false);
/* Our state newly carried by another rider: the status goes to the phone now, not in up to 10 s (D16). */
static atomic_bool s_status_now = ATOMIC_VAR_INIT(false);
static atomic_bool s_restart_after_bond_clear = ATOMIC_VAR_INIT(false);
static const char *s_reset = "other";

/* Console lines start with a word a bench script can look for: TX, RX, RIDER, STATUS, GNSS. */
static void console_line(const char *text) { port_console_line(text); }

/* A console line of our own, which only the cable had: kept for the phone's log too (LOG). */
static void note(const char *text) {
    console_line(text);
    port_mutex_lock(s_lock);
    api_log(&s_api, port_uptime_us(), text);
    port_mutex_give(s_lock);
}

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

/* GNSS 0|1: the kit's load switch on a Heltec (active low), the L76K's standby line
 * on an L1. */
static void gnss_power(bool on) {
#if defined(PIN_GNSS_EN)
    port_pin_set(PIN_GNSS_EN, on ? 0 : 1);
#elif defined(PIN_GNSS_WAKE)
    port_pin_set(PIN_GNSS_WAKE, on);
#else
    (void)on;
#endif
}

/* Runs one command and carries out what api_command leaves to its caller. True if it asked to
 * forget the paired phone, which only the transport knows how to answer. came_us: when the line
 * came, before any lock was waited for, which CLOCK answers with; all else is told as of the
 * moment the command runs, after any rider heard while it waited. */
static bool command(char *line, replies_t *out, int64_t came_us) {
    out->n = 0;
    port_mutex_lock(s_command_lock);
    port_mutex_lock(s_lock);
    api_command(&s_api, line, came_us, port_uptime_us(), gather, out);
    const bool forget = s_api.forget, restart = s_api.restart, bootloader = s_api.bootloader;
    if (s_api.gnss_ask) gnss_power(s_api.gnss_ask == 2);
    s_api.gnss_ask = 0;
    if (s_api.radio_changed) atomic_store(&s_reapply, true);
    setup_t setup = {.version = SETUP_VERSION, .keyed = s_node.cfg.keyed, .slot = s_node.cfg.slot,
                     .slots = s_node.cfg.slots, .interval_ms = s_node.cfg.interval_ms, .sf = s_node.cfg.phy.sf,
                     .tour_s = s_api.tour_s, .bw_khz = (uint16_t)(s_node.cfg.phy.bw_hz / 1000),
                     .power = s_api.power, .beat = s_node.cfg.beat, .passengers = s_node.cfg.passengers,
                     .move_m = s_node.cfg.move_m};
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
    port_mutex_give(s_command_lock);
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
    const int64_t came = port_uptime_us();
    static replies_t replies;
    char buf[128];
    if (memchr(line, '\0', len) || len >= sizeof(buf)) {
        ble_nus_send_line("ERR malformed command");
        return;
    }
    memcpy(buf, line, len + 1);
    if (command(buf, &replies, came)) {
        /* Firmware clears first, then FORGOTTEN tells the app it may drop its own bond too. */
        ble_nus_forget_bonds();
        return;
    }
    /* A reply goes whole or stops where a line did not: never its end after a gap, which the
     * phone would take for the whole of it. */
    for (int i = 0; i < replies.n; i++) {
        if (!ble_nus_send_line(replies.line[i])) {
            snprintf(buf, sizeof(buf), "WARN reply cut short: line %d of %d not sent", i + 1, replies.n);
            note(buf);
            break;
        }
    }
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
        if (len && command(buf, &replies, port_uptime_us())) {
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
    int64_t logged = -GNSS_LOG_US, pps = 0, told = -GNSS_TELL_US;
    uint8_t systems = 0;
    int64_t utc_ms = 0;
    bool was_valid = false;
    for (;;) {
        port_wdt_feed(watch);
        /* GGA and RMC only (the L76K's PCAS03): with GSA and GSV before it, the RMC that dates a
         * second came 0.1 s after it with no fix and over 0.8 s with twelve satellites (test plan
         * 2.1, 2026-11-11), which no fixed lag covers. Told again now and then, for a GNSS that
         * powers up after us. */
        if (port_uptime_us() - told >= GNSS_TELL_US) {
            port_gnss_write("$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n");
            told = port_uptime_us();
        }
        /* SYSTEMS (the L76K's PCAS04), once for each asking: not kept over a restart of ours,
         * for the bench. A byte read without the lock, which is whole or not yet there. */
        uint8_t want = s_api.gnss_systems;
        /* 8: GPS with BeiDou and GPS alone in turn, a quarter of an hour of UTC each, the first from
         * the hour and the half hour: the two compared under one sky by a board off its cable. */
        if (want == 8) want = !utc_ms ? systems : utc_ms / (15 * 60000) % 2 ? 1 : 3;
        if (want != systems) {
            systems = want;
            char body[12], cmd[20];
            const int n = snprintf(body, sizeof(body), "PCAS04,%u", systems);
            uint8_t sum = 0;
            for (int i = 0; i < n; i++) sum ^= (uint8_t)body[i];
            snprintf(cmd, sizeof(cmd), "$%s*%02X\r\n", body, sum);
            port_gnss_write(cmd);
        }
        const int c = port_gnss_getc(200);
        gnss_fix_t fix;
        if (c < 0 || !nmea_feed(&nmea, (char)c, &fix)) continue;
        /* ponytail: the fix's time is when its last sentence was parsed, after the UART's buffering;
         * a timestamp from the receive interrupt is finer, if E0 finds NMEA time too rough */
        if (fix.time_valid) utc_ms = fix.utc_ms;
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
            note(text);
            was_valid = fix.valid;
            logged = now;
        }
    }
}

/* ---- the radio -------------------------------------------------------------------- */

/* The mode asked for, to the radio, and the node told once the radio has it: until then it sends
 * nothing (ARCHITECTURE D15). A mode asked for again meanwhile is the next one applied. */
static bool apply_radio(void) {
    port_mutex_lock(s_lock);
    const phy_t want = s_node.cfg.phy;
    const uint8_t power = s_api.power;
    port_mutex_give(s_lock);
    if (!radio_apply(&want, power)) return false;
    port_mutex_lock(s_lock);
    node_phy_applied(&s_node, &want);
    port_mutex_give(s_lock);
    return true;
}

static void radio_task(void *arg) {
    (void)arg;
    const int watch = port_wdt_watch();
    radio_init();
    while (!apply_radio()) {
        note("WARN the radio will not take its configuration; trying again");
        port_wdt_feed(watch);
        port_sleep_ms(5000);
    }
    int64_t next_verify = port_uptime_us() + VERIFY_US;
    char text[API_LINE];
    /* a packet's sender and the riders it carried: static, as the task's stack has no room for them */
    static char rider_lines[1 + PKT_PASSENGERS_MAX][API_LINE];
    for (;;) {
        port_wdt_feed(watch);
        uint8_t buf[256];
        int rssi = 0, snr = 0;
        /* Listening until shortly before our slot, at most 20 ms at a time. */
        port_mutex_lock(s_lock);
        int64_t due = node_next_tx_us(&s_node, port_uptime_us());
        port_mutex_give(s_lock);
        int64_t wait_ms = (due - port_uptime_us()) / 1000 - 1;
        int64_t rx_at;
        const size_t n = radio_receive(buf, sizeof(buf), wait_ms < 0 ? 0 : wait_ms > 20 ? 20 : (uint32_t)wait_ms,
                                       &rssi, &snr, &rx_at);
        int64_t now = port_uptime_us();
        if (n) {
            port_mutex_lock(s_lock);
            const bool taken = node_on_packet(&s_node, buf, n, rssi, snr, now);
            /* E0's offset from its RxDone, where DIO1 stamped it; a poll's time is no measure */
            if (taken && rx_at) node_on_rx_done(&s_node, rx_at);
            /* A line to the phone for every rider the packet brought a newer record of: its sender,
             * first, and those it carried; our own state newly carried goes with the status, which
             * the status task sends in its order with the commands. */
            int lines = 0;
            const unsigned sender = buf[2] & 0x1F;
            for (unsigned i = 0; taken && i <= NODE_SLOTS && lines < (int)(sizeof(rider_lines) / sizeof(rider_lines[0])); i++) {
                const unsigned s = i == 0 ? sender : i - 1;
                if ((i == 0 || s != sender) && (s_node.rx_changed >> s & 1)) {
                    node_format_rider(&s_node, s, now, rider_lines[lines++], API_LINE);
                }
            }
            if (s_node.echo_news) atomic_store(&s_status_now, true);
            s_node.echo_news = false;
            port_mutex_give(s_lock);
            for (int i = 0; i < lines; i++) {
                if (ble_nus_connected()) ble_nus_send_line(rider_lines[i]);
                console_line(rider_lines[i]);
            }
            if (!taken) {
                snprintf(text, sizeof(text), "RX dropped len=%u rssi=%d", (unsigned)n, rssi);
                note(text);
            }
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
        const uint32_t counter = (uint8_t)(s_node.count - 1);
        port_mutex_give(s_lock);
        if (len) {
            int64_t done;
            const bool sent = radio_send(buf, len, &done);
            if (sent && done) {
                port_mutex_lock(s_lock);
                node_on_tx_done(&s_node, done);
                port_mutex_give(s_lock);
            }
            snprintf(text, sizeof(text), "TX counter=%lu len=%u %s", (unsigned long)counter, (unsigned)len,
                     sent ? "ok" : "FAILED");
            note(text);
        }
        now = port_uptime_us();
        port_mutex_lock(s_lock);
        const bool toured = api_tour(&s_api, now);
        port_mutex_give(s_lock);
        const bool reapply = atomic_exchange(&s_reapply, false) || toured;
        if (reapply || now >= next_verify) {
            /* a new power level, or a chip that no longer holds our configuration */
            if (reapply || !radio_verify()) apply_radio();
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
        /* LOG: the kept console lines to the phone, a few a tick, so a backlog leaves the link room
         * for the riders; under the command lock, so a LOG asked meanwhile starts where it says */
        port_mutex_lock(s_command_lock);
        port_mutex_lock(s_lock);
        if (!ble_nus_connected()) s_api.log_on = false;
        port_mutex_give(s_lock);
        for (int i = 0; i < 8; i++) {
            port_mutex_lock(s_lock);
            const bool more = api_log_peek(&s_api, text, sizeof(text));
            port_mutex_give(s_lock);
            if (!more || !ble_nus_send_line(text)) break;
            port_mutex_lock(s_lock);
            api_log_sent(&s_api);
            port_mutex_give(s_lock);
        }
        port_mutex_give(s_command_lock);
        if ((status_ms += 100) >= STATUS_PUSH_MS || atomic_exchange(&s_status_now, false)) {
            status_ms = 0;
            if (ble_nus_connected()) {
                port_mutex_lock(s_command_lock);
                port_mutex_lock(s_lock);
                api_status(&s_api, port_uptime_us(), text, sizeof(text));
                port_mutex_give(s_lock);
                ble_nus_send_line(text);
                port_mutex_give(s_command_lock);
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
                         .nmea_lag_sat_us = NODE_NMEA_LAG_SAT_US,
                         .tx_lead_us = NODE_TX_LEAD_US,
                         .listen_one_in = 32, .move_m = 10, .beat = 3, .passengers = 1,
                         .drift_ppm = NODE_DRIFT_PPM};
    setup_t setup;
    const bool v4 = port_setup_load(&setup, sizeof(setup)) && setup.version == SETUP_VERSION;
    const bool kept = (v4 || (port_setup_load(&setup, SETUP_V3_LEN) && setup.version == 3)) &&
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
    uint8_t power = NODE_POWER_LEVEL;
    if (v4 && kept) {
        /* each within what its command takes, else as built */
        if (setup.power <= 3) power = setup.power;
        if (setup.beat <= 7) cfg.beat = setup.beat;
        if (setup.passengers <= PKT_PASSENGERS_MAX) cfg.passengers = setup.passengers;
        if (setup.move_m >= 1 && setup.move_m <= 1000) cfg.move_m = setup.move_m;
    }
    const uint8_t tour_s = kept ? setup.tour_s : 0;
    memset(&setup, 0, sizeof(setup));
    s_lock = port_mutex_create();
    s_command_lock = port_mutex_create();
    node_init(&s_node, &cfg, port_random() ^ h);
    /* field by field: a compound literal of the whole, its log with it, would be built on this
     * task's small stack first */
    s_api.node = &s_node;
    s_api.board = board;
    s_api.reset = s_reset;
    s_api.power = power;
    s_api.tx_dbm = radio_tx_dbm;
    s_api.tour_s = tour_s;
    LOGI(TAG, "LoRa Convoy, protocol v%d, phone API %d, on %s after %s: %s, slot %u of %u%s, every %u ms, %d dBm",
         PKT_VERSION, API_PROTO, board, s_reset,
         cfg.keyed ? "keyed" : "no key yet: silent until the app or KEY gives one", cfg.slot, cfg.slots,
         kept ? " (kept)" : " (from the chip's ID; SLOT N changes it)", (unsigned)cfg.interval_ms,
         radio_tx_dbm(s_api.power));
    char boot[64];
    snprintf(boot, sizeof(boot), "BOOT board=%s reset=%s", board, s_reset);
    note(boot);
    ble_nus_init(on_ble_line, on_ble_revoke, on_ble_bonds_cleared);
    port_task_create(gnss_task, "gnss", 4096, 4);
    port_task_create(radio_task, "radio", 6144, 5);
    port_task_create(console_task, "console", 4096, 3);
    port_task_create(service_task, "service", 4096, 3);
}
