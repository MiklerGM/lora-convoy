/*
 * The node core on the host: airtime against tools/airtime.py, the packet against a fixed vector,
 * NMEA as an L76K sends it, and the node's rules for sending and receiving.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "codec.h"
#include "nmea.h"
#include "node.h"
#include "phy.h"

#define SEC 1000000ll

/* ---- phy ------------------------------------------------------------------------- */

static void test_airtime(void) {
    /* tools/airtime.py time_on_air(payload, sf, bw_khz, cr, preamble), in microseconds */
    static const struct { int len, sf; uint32_t bw; int cr; uint32_t us; } cases[] = {
        {17, 7, 250000, 5, 25728},   {24, 7, 250000, 5, 30848},   {17, 9, 250000, 5, 82432},
        {17, 10, 125000, 5, 329728}, {17, 11, 125000, 5, 659456}, {17, 12, 125000, 5, 1318912},
        {255, 7, 250000, 5, 199808}, {1, 7, 250000, 8, 14464},    {17, 7, 62500, 5, 102912},
        {17, 12, 250000, 5, 659456}, {24, 10, 125000, 5, 370688},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        phy_t p = PHY_V0;
        p.sf = (uint8_t)cases[i].sf;
        p.bw_hz = cases[i].bw;
        p.cr = (uint8_t)cases[i].cr;
        if (phy_airtime_us(&p, (size_t)cases[i].len) != cases[i].us) {
            fprintf(stderr, "airtime case %zu: %u, want %u\n", i, phy_airtime_us(&p, (size_t)cases[i].len),
                    cases[i].us);
            abort();
        }
    }
    phy_t p = PHY_V0;
    assert(phy_supported(&p) && !phy_ldro(&p));
    p.sf = 11, p.bw_hz = 125000;
    assert(phy_ldro(&p)); /* 16.384 ms symbols */
    p.sf = 10;
    assert(!phy_ldro(&p));
    p.bw_hz = 200000;
    assert(!phy_supported(&p));
    printf("phy: airtime matches tools/airtime.py in %zu cases\n", sizeof(cases) / sizeof(cases[0]));
}

/* ---- crypto ---------------------------------------------------------------------- */

static void test_crypto(void) {
    /* FIPS-197, appendix C.1 */
    static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    aes128_t aes;
    uint8_t out[32];
    aes128_init(&aes, key);
    aes128_encrypt(&aes, pt, out);
    assert(memcmp(out, ct, 16) == 0);

    /* RFC 3610, packet vector 1: 8 bytes of associated data, 23 of message, an 8-byte tag */
    uint8_t k3610[16], msg[31];
    for (int i = 0; i < 16; i++) k3610[i] = (uint8_t)(0xC0 + i);
    for (int i = 0; i < 31; i++) msg[i] = (uint8_t)i;
    static const uint8_t nonce[CCM_NONCE_LEN] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
                                                 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    static const uint8_t want1[31] = {0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2, 0xF0, 0x66, 0xD0,
                                      0xC2, 0xC0, 0xF9, 0x89, 0x80, 0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3,
                                      0x84, 0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0};
    aes128_init(&aes, k3610);
    ccm_seal(&aes, nonce, msg, 8, &msg[8], 23, out, &out[23], 8);
    assert(memcmp(out, want1, sizeof(want1)) == 0);
    uint8_t back[23];
    assert(ccm_open(&aes, nonce, msg, 8, out, 23, &out[23], 8, back) && memcmp(back, &msg[8], 23) == 0);
    out[3] ^= 0x01; /* one bit of ciphertext: refused, and nothing comes out */
    assert(!ccm_open(&aes, nonce, msg, 8, out, 23, &out[23], 8, back) && back[0] == 0 && back[22] == 0);

    /* The shape v1 uses - 9 bytes associated, 11 of message, a 4-byte tag - from OpenSSL. */
    static const uint8_t want2[15] = {0x59, 0x8F, 0x96, 0x9D, 0x60, 0xC5, 0x62, 0xCD,
                                      0xF1, 0x65, 0xD1, 0x55, 0x6F, 0xE1, 0x87};
    ccm_seal(&aes, nonce, msg, 9, &msg[9], 11, out, &out[11], 4);
    assert(memcmp(out, want2, sizeof(want2)) == 0);
    printf("crypto: AES-128 and CCM match FIPS-197, RFC 3610 and OpenSSL\n");
}

/* ---- codec ----------------------------------------------------------------------- */

static const uint8_t KEY[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static void test_codec(void) {
    /* The vector in docs/PROTOCOL.md, from an independent implementation on OpenSSL: slot 3,
     * session 0x01020304, counter 5, a fix at 2026-10-22 12:00:05 UTC, calling for help in its call
     * 5 and hearing slot 7's call 2, under the key 00 01 .. 0f. */
    static const uint8_t want[PKT_POSITION_LEN] = {0xC0, 0x86, 0x03, 0x01, 0x02, 0x03, 0x04, 0x00, 0x05,
                                                   0xEE, 0x37, 0x9D, 0x2B, 0x30, 0xA8, 0x88, 0x6B, 0x0E,
                                                   0x6D, 0x23, 0x0D, 0x03, 0x40, 0x86, 0x0F};
    group_key_t g;
    group_key_init(&g, KEY);
    assert(g.tag == 0x86);
    const position_t p = {.slot = 3, .session = 0x01020304, .counter = 5, .fix_time = 64197,
                          .lat_e7 = 471410000, .lon_e7 = 95215000, .speed_kmh = 40,
                          .course2 = 135, .status = ST_GPS_OK | ST_HELP, .call = 5, .help_heard = 7,
                          .heard_call = 2};
    uint8_t b[PKT_POSITION_LEN];
    assert(pkt_seal(b, &p, &g) == PKT_POSITION_LEN && memcmp(b, want, sizeof(want)) == 0);

    position_t q;
    assert(pkt_open(b, sizeof(b), &g, &q) == PKT_OK);
    assert(q.group == 0x86 && q.slot == 3 && q.session == 0x01020304 && q.counter == 5 && q.fix_time == 64197);
    assert(q.speed_kmh == 40 && q.course2 == 135 && q.status == (ST_GPS_OK | ST_HELP) && q.call == 5);
    assert(q.help_heard == 7 && q.heard_call == 2);
    /* half a step at most: 1.07e-5 degrees of latitude, 2.15e-5 of longitude */
    assert(labs((long)q.lat_e7 - 471410000) <= 54 && labs((long)q.lon_e7 - 95215000) <= 108);

    /* the corners of the world, and a value just below zero */
    static const int32_t lats[] = {900000000, -900000000, -1, 0};
    static const int32_t lons[] = {1800000000, -1800000000, -1, 0};
    for (size_t i = 0; i < 4; i++) {
        position_t c = p;
        c.lat_e7 = lats[i];
        c.lon_e7 = lons[i];
        pkt_seal(b, &c, &g);
        assert(pkt_open(b, sizeof(b), &g, &q) == PKT_OK);
        assert(labs((long)q.lat_e7 - lats[i]) <= 108 && labs((long)q.lon_e7 - lons[i]) <= 215);
    }

    /* Every byte is covered: the clear header, the body and the tag. */
    for (size_t i = 0; i < sizeof(b); i++) {
        pkt_seal(b, &p, &g);
        b[i] ^= 0x20;
        const pkt_result_t r = pkt_open(b, sizeof(b), &g, &q);
        assert(i == 0 ? r == PKT_MALFORMED : i == 1 ? r == PKT_FOREIGN : r == PKT_FORGED);
    }
    /* Another group's key: its tag differs, and so would its authentication. */
    uint8_t other[16];
    memcpy(other, KEY, 16);
    other[15] ^= 1;
    group_key_t h;
    group_key_init(&h, other);
    pkt_seal(b, &p, &h);
    assert(h.tag != g.tag && pkt_open(b, sizeof(b), &g, &q) == PKT_FOREIGN);
    b[1] = g.tag; /* even wearing our tag */
    assert(pkt_open(b, sizeof(b), &g, &q) == PKT_FORGED);
    assert(pkt_open(b, sizeof(b) - 1, &g, &q) == PKT_MALFORMED);
    printf("codec: the fixed vector, the world's corners, and every byte authenticated\n");
}

/* ---- nmea ------------------------------------------------------------------------- */

/* Feeds "$" body "*" checksum, computing the checksum unless one is given. */
static bool feed(nmea_t *n, const char *body, const char *checksum, gnss_fix_t *fix) {
    char line[160];
    uint8_t sum = 0;
    for (const char *p = body; *p; p++) sum ^= (uint8_t)*p;
    if (checksum) {
        snprintf(line, sizeof(line), "$%s*%s\r\n", body, checksum);
    } else {
        snprintf(line, sizeof(line), "$%s*%02X\r\n", body, sum);
    }
    bool done = false;
    for (const char *p = line; *p; p++) done |= nmea_feed(n, *p, fix);
    return done;
}

static void test_nmea(void) {
    nmea_t n;
    gnss_fix_t fix;
    nmea_init(&n);
    /* An L76K's second, in its own order: GGA before RMC, and sentences we do not read. */
    assert(!feed(&n, "GNGGA,095528.000,2232.1912,N,11356.6574,E,1,13,0.9,59.6,M,0.0,M,,", NULL, &fix));
    assert(!feed(&n, "GNGSA,A,3,10,12,14,25,31,32,,,,,,,1.6,0.9,1.3,1", NULL, &fix));
    assert(!feed(&n, "GPGSV,3,1,10,10,32,186,40,12,41,323,38,14,34,066,35,22,07,224,,0", NULL, &fix));
    assert(!feed(&n, "GPTXT,01,01,01,ANTENNA OK", NULL, &fix));
    assert(feed(&n, "GNRMC,095528.000,A,2232.1912,N,11356.6574,E,0.21,201.08,140821,,,A,V", NULL, &fix));
    assert(fix.valid && fix.time_valid && fix.utc_ms == 1628934928000ll);
    assert(fix.lat_e7 == 225365200 && fix.lon_e7 == 1139442900);
    assert(fix.speed_kmh10 == 4 && fix.course_deg10 == 2010 && fix.sats == 13 && fix.hdop10 == 9);

    /* The other hemispheres, and fractions of a second. */
    assert(feed(&n, "GPRMC,120005.50,A,4708.4600,S,00931.2900,W,21.6,270.0,221026,,,D", NULL, &fix));
    assert(fix.valid && fix.utc_ms == 1792670405500ll);
    assert(fix.lat_e7 == -471410000 && fix.lon_e7 == -95215000);
    assert(fix.speed_kmh10 == 400 && fix.course_deg10 == 2700);

    /* Time before a fix: the date and time are good, the position is not. */
    assert(feed(&n, "GNRMC,120006.000,V,,,,,,,221026,,,N,V", NULL, &fix));
    assert(fix.time_valid && !fix.valid && fix.utc_ms == 1792670406000ll);
    /* A status of A with a mode of N is no fix either. */
    assert(feed(&n, "GNRMC,120007.000,A,4708.4600,N,00931.2900,E,0.0,,221026,,,N,V", NULL, &fix));
    assert(!fix.valid && fix.course_deg10 == -1);

    /* A wrong checksum, a cut line and noise are dropped; the next good sentence still reads. */
    const uint32_t bad = n.bad;
    assert(!feed(&n, "GNRMC,120008.000,A,4708.4600,N,00931.2900,E,0.0,0.0,221026,,,A,V", "00", &fix));
    assert(n.bad == bad + 1);
    for (const char *p = "$GNRMC,120009.000,A,47"; *p; p++) assert(!nmea_feed(&n, *p, &fix));
    for (int i = 0; i < 300; i++) assert(!nmea_feed(&n, (char)('A' + i % 26), &fix));
    assert(feed(&n, "GNRMC,120010.000,A,4708.4600,N,00931.2900,E,0.0,0.0,221026,,,A,V", NULL, &fix));
    assert(fix.valid && fix.utc_ms == 1792670410000ll);
    /* Out of range: 95 degrees of latitude is no position. */
    assert(feed(&n, "GNRMC,120011.000,A,9500.0000,N,00931.2900,E,0.0,0.0,221026,,,A,V", NULL, &fix));
    assert(!fix.valid && fix.time_valid);
    printf("nmea: an L76K's sentences, hemispheres, time before a fix, and what is refused\n");
}

/* ---- node ------------------------------------------------------------------------- */

/* Slot 1 of 4 riders every 2 s: five spacings of 400 ms, the last one spare. An NMEA-only board
 * whose fixes arrive on the second, give or take 1 ms. */
static const node_config_t CFG = {.keyed = true,
                                  .key = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                          0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
                                  .slot = 1, .slots = 4, .interval_ms = 2000, .duty_permille = 100,
                                  .phy = {869525000, 250000, 7, 5, 8, {0x64, 0x74}},
                                  .nmea_lag_us = 0, .nmea_err_us = 1000};

static gnss_fix_t fix_at(int64_t utc_ms, bool valid) {
    return (gnss_fix_t){.time_valid = true, .valid = valid, .utc_ms = utc_ms, .lat_e7 = 471410000,
                        .lon_e7 = 95215000, .speed_kmh10 = 400, .course_deg10 = 2700, .sats = 9};
}

/* 12:00:05 UTC on 2026-10-22, at 10 s on the board's clock: its fixes come each second from then. */
#define UTC0_MS 1792670405000ll
#define T0 (10 * SEC)

static bool starts(const char *s, const char *prefix);

static void tick(node_t *n, int64_t t) {
    if (t >= T0 && t % SEC == 0) {
        gnss_fix_t f = fix_at(UTC0_MS + (t - T0) / 1000, true);
        node_on_fix(n, &f, t);
    }
}

static void test_node_sends(void) {
    node_t n;
    uint8_t b[32];
    group_key_t g;
    group_key_init(&g, KEY);
    node_init(&n, &CFG, 12345);
    /* Silent before its GNSS gives a time: no slot can be told, nothing is counted. */
    for (int64_t t = 0; t < T0; t += 1000) assert(node_poll_tx(&n, t, b, sizeof(b)) == 0);
    assert(node_next_tx_us(&n, 0) == INT64_MAX && n.tx_skipped == 0);

    /* Then once a frame, from the frame after the time came, inside slot 1: a spacing on from the
     * frame's start plus its phase, which is under a spacing and differs from frame to frame. */
    position_t p;
    int sent = 0;
    int64_t t = T0, last_k = -1, phase_min = SEC, phase_max = 0;
    for (; t < 70 * SEC; t += 1000) {
        tick(&n, t);
        const size_t len = node_poll_tx(&n, t, b, sizeof(b));
        if (!len) continue;
        assert(len == PKT_POSITION_LEN && pkt_open(b, len, &g, &p) == PKT_OK);
        assert(p.counter == (uint16_t)sent && p.session == n.session && p.slot == 1 && (p.status & ST_GPS_OK));
        const int64_t utc = UTC0_MS * 1000 + (t - T0), k = utc / 2000000, phase = utc % 2000000 - 400000;
        assert(last_k < 0 ? k == (UTC0_MS * 1000) / 2000000 + 1 : k == last_k + 1);
        assert(phase >= 0 && phase < 400000 + 1000);
        if (phase < phase_min) phase_min = phase;
        if (phase > phase_max) phase_max = phase;
        last_k = k;
        sent++;
    }
    assert(sent == 30 && phase_max - phase_min > 100000);
    assert(n.tx_late == 0 && n.tx_unsure == 0 && n.tx_skipped == 0);

    /* The platform is told when to be ready: not a moment early, and too late is passed over. */
    tick(&n, t);
    int64_t due = node_next_tx_us(&n, t);
    assert(due > t && due - t <= 2 * SEC);
    assert(node_poll_tx(&n, due - 1, b, sizeof(b)) == 0);
    assert(node_poll_tx(&n, due + 3000, b, sizeof(b)) == 0 && n.tx_late == 1);
    /* on whole milliseconds again, so the fixes keep coming on the second */
    for (t = (due / 1000 + 4) * 1000; (due = node_next_tx_us(&n, t)) > t + 1000; t += 1000) tick(&n, t);
    assert(node_poll_tx(&n, due, b, sizeof(b)) == PKT_POSITION_LEN);
    t = due;

    /* The fix is lost: the held position goes on, with its old time and without GPS_OK. */
    t = (t / SEC + 1) * SEC;
    gnss_fix_t lost = fix_at(UTC0_MS + (t - T0) / 1000, false);
    node_on_fix(&n, &lost, t);
    const int64_t last_valid = UTC0_MS + (t - T0) / 1000 - 1000;
    for (const int64_t end = t + 10 * SEC; t < end; t += 1000) {
        if (node_poll_tx(&n, t, b, sizeof(b))) {
            assert(pkt_open(b, PKT_POSITION_LEN, &g, &p) == PKT_OK && !(p.status & ST_GPS_OK));
            assert(p.fix_time == (uint16_t)(last_valid / 1000));
        }
    }

    /* The counter never wraps within a session: its last value, then a new session. */
    n.counter = 0xFFFF;
    const uint32_t before = n.session;
    while (!node_poll_tx(&n, t, b, sizeof(b))) t += 1000;
    assert(pkt_open(b, PKT_POSITION_LEN, &g, &p) == PKT_OK && p.counter == 0xFFFF && p.session == before);
    while (!node_poll_tx(&n, t, b, sizeof(b))) t += 1000;
    assert(pkt_open(b, PKT_POSITION_LEN, &g, &p) == PKT_OK && p.counter == 0 && p.session != before);

    /* Without a key, nothing goes out at all: v1 has no clear text. */
    node_config_t bare = CFG;
    bare.keyed = false;
    node_t quiet;
    node_init(&quiet, &bare, 1);
    for (int64_t s = T0; s < 20 * SEC; s += 1000) {
        tick(&quiet, s);
        assert(node_poll_tx(&quiet, s, b, sizeof(b)) == 0);
    }
    /* Nor from a slot outside the group. */
    node_config_t outside = CFG;
    outside.slot = 4;
    node_init(&quiet, &outside, 1);
    for (int64_t s = T0; s < 20 * SEC; s += 1000) {
        tick(&quiet, s);
        assert(node_poll_tx(&quiet, s, b, sizeof(b)) == 0);
    }
    /* Now and then it listens in its slot instead, at random: once in four frames here. */
    node_config_t listener = CFG;
    listener.listen_one_in = 4;
    node_init(&quiet, &listener, 1);
    int heard_out = 0;
    for (int64_t s = T0; s < T0 + 800 * SEC; s += 1000) {
        tick(&quiet, s);
        heard_out += node_poll_tx(&quiet, s, b, sizeof(b)) > 0;
    }
    assert(quiet.tx_listened > 70 && quiet.tx_listened < 130 && heard_out + (int)quiet.tx_listened == 400);
    /* Nor while its time is less sure than the slots allow: 200 ms against a budget of 183.6. */
    node_config_t rough = CFG;
    rough.nmea_err_us = 200000;
    node_init(&quiet, &rough, 1);
    for (int64_t s = T0; s < 30 * SEC; s += 1000) {
        tick(&quiet, s);
        assert(node_poll_tx(&quiet, s, b, sizeof(b)) == 0);
    }
    assert(quiet.tx_unsure >= 9 && quiet.tx_unsure <= 10);
    printf("node: once a frame in its slot, from a phase drawn per frame; silent without a key, a time "
           "good enough, or a slot in the group\n");
}

static void test_node_time(void) {
    node_config_t cfg = CFG;
    cfg.nmea_lag_us = 100000;
    cfg.nmea_err_us = 20000;
    node_t n;
    node_init(&n, &cfg, 1);
    int64_t utc, err;
    assert(!node_utc_us(&n, 0, &utc, &err));

    /* NMEA alone: the fix's second was its arrival less the lag configured. */
    gnss_fix_t f = fix_at(UTC0_MS, true);
    node_on_fix(&n, &f, 5150000);
    assert(n.time == TIME_NMEA && node_utc_us(&n, 5150000, &utc, &err));
    assert(utc == UTC0_MS * 1000 + 100000 && err == 20000 + 5);

    /* A PPS edge, then the fix that names its second: the edge is the second, and how far the NMEA
     * lagged behind it is measured. */
    node_on_pps(&n, 6000000);
    f = fix_at(UTC0_MS + 1000, true);
    node_on_fix(&n, &f, 6130000);
    assert(n.time == TIME_PPS && node_utc_us(&n, 6130000, &utc, &err));
    assert(utc == (UTC0_MS + 1000) * 1000 + 130000 && err == 100 + 6 && n.lag_us == 130000);

    /* An edge missed: a fix 1.13 s after the last edge is not its second, and NMEA's time is worse
     * than the edge's carried a second. */
    f = fix_at(UTC0_MS + 2000, true);
    node_on_fix(&n, &f, 7130000);
    assert(n.time == TIME_PPS && n.mark_us == 6000000);

    /* Holding over without PPS: the edge's time grows 50 ppm less sure, and once that is worse than
     * NMEA's 20 ms, some 400 s on, NMEA's is taken. */
    f = fix_at(UTC0_MS + 300000, true);
    node_on_fix(&n, &f, 305130000);
    assert(n.time == TIME_PPS);
    f = fix_at(UTC0_MS + 500000, true);
    node_on_fix(&n, &f, 505130000);
    assert(n.time == TIME_NMEA && n.mark_us == 505030000);

    /* The budget: 400 ms between slots, less the packet's 30.8 ms and 2 ms for the receivers,
     * halved between the two senders either side. */
    assert(node_time_budget_us(&n) == (400000 - 30848 - 2000) / 2);
    n.cfg.slots = 32;
    n.cfg.interval_ms = 500; /* 15 ms between slots: no room for a packet */
    assert(node_time_budget_us(&n) == 0);
    printf("node: time from PPS edges or NMEA, the better one kept, its uncertainty growing in holdover\n");
}

static void test_node_duty(void) {
    /* 1 per mille is 3.6 s an hour: 116 packets of 31 ms, and then silence. */
    node_config_t cfg = CFG;
    cfg.duty_permille = 1;
    cfg.interval_ms = 1000;
    node_t n;
    uint8_t b[32];
    node_init(&n, &cfg, 1);
    int sent = 0;
    for (int64_t t = T0; t < 30 * 60 * SEC; t += 1000) {
        tick(&n, t);
        sent += node_poll_tx(&n, t, b, sizeof(b)) > 0;
    }
    assert(sent == 116 && node_duty_used_ms(&n, 30 * 60 * SEC) == 116 * 31);
    assert(n.tx_skipped > 1000);
    /* The hour, to the minute: what was booked in minute 0 counts through minute 59, not 60. */
    node_t one;
    node_init(&one, &cfg, 1);
    int64_t t = T0;
    for (; !node_poll_tx(&one, t, b, sizeof(b)); t += 1000) tick(&one, t);
    assert(t < 60 * SEC);
    assert(node_duty_used_ms(&one, 59 * 60 * SEC + 59 * SEC) == 31);
    assert(node_duty_used_ms(&one, 60 * 60 * SEC) == 0);
    /* The ledger: the hour a minute at a time, oldest first; the first minute's 116 packets. */
    char ledger[API_LINE];
    node_format_ledger(&n, 30 * 60 * SEC, ledger, sizeof(ledger));
    assert(starts(ledger, "LEDGER minutes=60 limit_ms=3600 used_ms=3596 positions_ms=0,"));
    assert(strstr(ledger, ",0,0,") && strlen(ledger) < API_LINE);
    int sum = 0;
    for (const char *c = strstr(ledger, "positions_ms=") + 13; *c; c++) {
        sum += atoi(c);
        while (*c && *c != ',') c++;
        if (!*c) break;
    }
    assert(sum == 3596);
    /* An hour after the first packets the budget comes back. */
    int later = 0;
    for (t = 70 * 60 * SEC; t < 75 * 60 * SEC; t += 1000) {
        tick(&n, t);
        later += node_poll_tx(&n, t, b, sizeof(b)) > 0;
    }
    assert(later > 100);
    printf("node: the duty cycle stops it at the limit, and the budget returns after the hour\n");
}

static size_t packet(uint8_t *b, const group_key_t *g, uint8_t slot, uint32_t session, uint16_t counter,
                     uint16_t fix_time) {
    const position_t p = {.slot = slot, .session = session, .counter = counter, .fix_time = fix_time,
                          .lat_e7 = 471500000, .lon_e7 = 95100000, .speed_kmh = 33, .course2 = 10,
                          .status = ST_GPS_OK, .help_heard = HELP_HEARD_NONE};
    return pkt_seal(b, &p, g);
}

static void test_node_hears(void) {
    node_t n;
    uint8_t b[PKT_POSITION_LEN];
    char line[400];
    group_key_t g, other;
    group_key_init(&g, KEY);
    uint8_t other_key[16] = {0};
    group_key_init(&other, other_key);
    node_init(&n, &CFG, 7);
    const int64_t utc0 = 1792670405000ll; /* 12:00:05, 64197 modulo 65536 */
    gnss_fix_t f = fix_at(utc0, true);
    node_on_fix(&n, &f, 0);

    /* Rider 2, its fix 5 s before ours, heard 3 s after our fix. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, 10, 64192), -71, 33, 3 * SEC));
    node_format_rider(&n, 2, 3 * SEC, line, sizeof(line));
    assert(strstr(line, "fix_age=8 ") && strstr(line, "lat=47.15000") && strstr(line, "rssi=-71 "));
    assert(strstr(line, "snr=8.25 ") && strstr(line, "speed=33 course=20 gps=1"));

    /* Delivered share: counters 10, 11, 13 make three of four; 11 again is a repeat. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, 11, 64194), -70, 30, 5 * SEC));
    assert(!node_on_packet(&n, b, packet(b, &g, 2, 100, 11, 64194), -70, 30, 5 * SEC) && n.rx_replay == 1);
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, 13, 64198), -70, 30, 7 * SEC));
    node_format_rider(&n, 2, 7 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=3/4"));
    /* It restarted: a new session, a fresh fix, and its run starts again. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 200, 0, 64200), -70, 30, 9 * SEC));
    node_format_rider(&n, 2, 9 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=1/1"));
    /* The old session played back: its fix is older than the one we hold, so it is not taken. */
    assert(!node_on_packet(&n, b, packet(b, &g, 2, 100, 14, 64199 - 3), -70, 30, 9 * SEC) && n.rx_replay == 2);

    /* A rider that restarted within the second: both sessions' packets are genuine, and played
     * back in turn, each would look like a new session to a receiver that remembered only the
     * last one. Two hundred of them, and none is taken. */
    node_t twice;
    node_init(&twice, &CFG, 9);
    node_on_fix(&twice, &f, 0);
    uint8_t rec[4][PKT_POSITION_LEN];
    packet(rec[0], &g, 6, 300, 10, 64197);
    packet(rec[1], &g, 6, 300, 11, 64198);
    packet(rec[2], &g, 6, 400, 0, 64198);
    packet(rec[3], &g, 6, 400, 1, 64199);
    for (int k = 0; k < 4; k++) assert(node_on_packet(&twice, rec[k], PKT_POSITION_LEN, -70, 30, k * SEC));
    for (int k = 0; k < 200; k++) {
        assert(!node_on_packet(&twice, rec[k % 2 ? 3 : 1], PKT_POSITION_LEN, -70, 30, (5 + k) * SEC));
    }
    assert(twice.rx_replay == 200);

    /* A fix stamped in the future (its clock is wrong) shows as such, not as fresh. */
    node_on_packet(&n, b, packet(b, &g, 3, 1, 0, 64197 + 20), -80, 0, 2 * SEC);
    node_format_rider(&n, 3, 2 * SEC, line, sizeof(line));
    assert(strstr(line, "fix_age=-18 "));
    /* 65000 s ago reads as 536 s ahead: past half its range the nearest wins (RFC 1982). */
    node_on_packet(&n, b, packet(b, &g, 4, 1, 0, (uint16_t)(64197 - 65000)), -80, 0, 0);
    node_format_rider(&n, 4, 0, line, sizeof(line));
    assert(strstr(line, "fix_age=65000 ") == NULL && strstr(line, "fix_age=-536 "));

    /* Another group, a forgery, our own slot and noise are counted and kept out. */
    assert(!node_on_packet(&n, b, packet(b, &other, 5, 1, 0, 64197), -80, 0, 0) && n.rx_foreign == 1);
    packet(b, &g, 5, 1, 0, 64197);
    b[12] ^= 0x01;
    assert(!node_on_packet(&n, b, PKT_POSITION_LEN, -80, 0, 0) && n.rx_forged == 1);
    assert(!node_on_packet(&n, b, packet(b, &g, 1, 1, 0, 64197), -80, 0, 0) && n.rx_clash == 1);
    assert(!node_on_packet(&n, b, 5, -80, 0, 0) && n.rx_bad == 1);
    assert(!n.riders[5].known && !n.riders[1].known);

    /* Without a time of our own, an age cannot be told: it says so. */
    node_t blind;
    node_init(&blind, &CFG, 7);
    node_on_packet(&blind, b, packet(b, &g, 2, 1, 0, 64192), -71, 33, 0);
    node_format_rider(&blind, 2, 0, line, sizeof(line));
    assert(strstr(line, "fix_age=? "));
    node_format_status(&n, 9 * SEC, line, sizeof(line));
    assert(strncmp(line, "slot=1 slots=4 keyed=1 group=86 mode=7/250 fix=0 ", 49) == 0);
    /* whom it knows, by slot: what the app keeps on its map, whatever lines it missed */
    assert(strstr(line, " riders=3 known=0000001c "));
    assert(strstr(line, " time=nmea err=1.") && strstr(line, " budget=183.5 lag=? "));
    assert(strstr(line, " foreign=1 forged=1 replay=2 clash=1"));
    printf("node: ages honest, the delivered share, and forgeries, replays and strangers kept out\n");
}

static void test_node_help(void) {
    uint8_t b[PKT_POSITION_LEN];
    char line[400];
    position_t p;
    group_key_t g;
    group_key_init(&g, KEY);
    node_t a, c, w;
    node_config_t ca = CFG, cc = CFG, cw = CFG;
    ca.slot = 1;
    cc.slot = 2;
    cw.slot = 3;
    node_init(&a, &ca, 1);
    node_init(&c, &cc, 2);
    node_init(&w, &cw, 5);
    /* A asks for help. C hears it, and answers in its next packet; A hears the answer, and so does
     * W, which tells its phone who has answered A. */
    node_set_help(&a, true);
    int64_t t = T0;
    size_t len = 0;
    for (; !len; t += 1000) {
        tick(&a, t);
        tick(&c, t);
        len = node_poll_tx(&a, t, b, sizeof(b));
    }
    assert(pkt_open(b, len, &g, &p) == PKT_OK && (p.status & ST_HELP) && p.help_heard == HELP_HEARD_NONE);
    assert(node_on_packet(&c, b, len, -80, 0, t) && node_on_packet(&w, b, len, -80, 0, t));
    for (len = 0; !len; t += 1000) {
        tick(&c, t);
        len = node_poll_tx(&c, t, b, sizeof(b));
    }
    assert(pkt_open(b, len, &g, &p) == PKT_OK && !(p.status & ST_HELP) && p.help_heard == 1);
    assert(p.heard_call == a.help_call);
    assert(node_on_packet(&a, b, len, -80, 0, t) && a.help_heard_by == 1u << 2);
    assert(node_on_packet(&w, b, len, -80, 0, t));
    node_format_rider(&w, 1, t, line, sizeof(line));
    assert(strstr(line, " help=1 stopped=0 answered=2 "));
    /* An answer to another rider's call is not one to ours. */
    const position_t other = {.slot = 4, .session = 77, .counter = 0, .fix_time = 64197, .lat_e7 = 1, .lon_e7 = 1,
                              .status = ST_GPS_OK, .help_heard = 5, .heard_call = a.help_call};
    pkt_seal(b, &other, &g);
    assert(node_on_packet(&a, b, PKT_POSITION_LEN, -80, 0, t) && a.help_heard_by == 1u << 2);
    node_format_status(&a, t, line, sizeof(line));
    assert(strstr(line, " help=1 heard=1 "));
    /* Called off and on again, it starts afresh: C's next packet, sent before C has heard the new
     * call, answers the one called off and does not count; C's answer after hearing it does. */
    node_set_help(&a, false);
    node_set_help(&a, true);
    assert(a.help_heard_by == 0);
    for (len = 0; !len; t += 1000) {
        tick(&c, t);
        len = node_poll_tx(&c, t, b, sizeof(b));
    }
    assert(node_on_packet(&a, b, len, -80, 0, t) && a.help_heard_by == 0);
    for (len = 0; !len; t += 1000) {
        tick(&a, t);
        len = node_poll_tx(&a, t, b, sizeof(b));
    }
    /* W too: the new call has no answer yet. */
    assert(node_on_packet(&c, b, len, -80, 0, t) && node_on_packet(&w, b, len, -80, 0, t));
    node_format_rider(&w, 1, t, line, sizeof(line));
    assert(strstr(line, " help=1 stopped=0 answered=- "));
    for (len = 0; !len; t += 1000) {
        tick(&c, t);
        len = node_poll_tx(&c, t, b, sizeof(b));
    }
    assert(node_on_packet(&a, b, len, -80, 0, t) && a.help_heard_by == 1u << 2);
    assert(node_on_packet(&w, b, len, -80, 0, t));
    node_format_rider(&w, 1, t, line, sizeof(line));
    assert(strstr(line, " help=1 stopped=0 answered=2 "));
    /* A new key is another group: no one in it has answered yet. */
    static const uint8_t key2[16] = {0x0f, 0x0e};
    node_set_key(&a, key2);
    node_format_status(&a, t, line, sizeof(line));
    assert(strstr(line, " help=1 heard=0 "));

    /* Two calling at once are answered in turn; a call not heard for a minute is not answered. */
    node_t q;
    node_init(&q, &cc, 3);
    tick(&q, T0);
    const position_t call1 = {.slot = 1, .session = 9, .counter = 0, .fix_time = 64197, .lat_e7 = 1, .lon_e7 = 1,
                              .status = ST_HELP | ST_GPS_OK, .call = 1, .help_heard = HELP_HEARD_NONE};
    position_t call5 = call1;
    call5.slot = 5;
    pkt_seal(b, &call1, &g);
    assert(node_on_packet(&q, b, PKT_POSITION_LEN, -80, 0, T0));
    pkt_seal(b, &call5, &g);
    assert(node_on_packet(&q, b, PKT_POSITION_LEN, -80, 0, T0));
    int answered[2] = {0, 0}, n_sent = 0;
    for (t = T0; n_sent < 4; t += 1000) {
        tick(&q, t);
        if (!(len = node_poll_tx(&q, t, b, sizeof(b)))) continue;
        assert(pkt_open(b, len, &g, &p) == PKT_OK && (p.help_heard == 1 || p.help_heard == 5));
        answered[p.help_heard == 5]++;
        n_sent++;
    }
    assert(answered[0] == 2 && answered[1] == 2);
    for (; t < T0 + 62 * SEC; t += 1000) tick(&q, t), node_poll_tx(&q, t, b, sizeof(b));
    for (len = 0; !len; t += 1000) {
        tick(&q, t);
        len = node_poll_tx(&q, t, b, sizeof(b));
    }
    assert(pkt_open(b, len, &g, &p) == PKT_OK && p.help_heard == HELP_HEARD_NONE);

    /* Stopped: under 5 km/h for a minute; not at 7 km/h after that; moving again over 10 km/h. */
    node_t s;
    node_init(&s, &CFG, 4);
    gnss_fix_t f = fix_at(UTC0_MS, true);
    f.speed_kmh10 = 20;
    for (t = T0; t < T0 + 59 * SEC; t += SEC) node_on_fix(&s, &f, t);
    assert(!s.stopped);
    node_on_fix(&s, &f, T0 + 60 * SEC);
    assert(s.stopped);
    f.speed_kmh10 = 70;
    node_on_fix(&s, &f, T0 + 61 * SEC);
    assert(s.stopped);
    f.speed_kmh10 = 110;
    node_on_fix(&s, &f, T0 + 62 * SEC);
    assert(!s.stopped);
    printf("node: a call for help answered by those who hear it, each in turn; stopped after a minute\n");
}

/* ---- the phone API ------------------------------------------------------------------ */

static char replies[8][400];
static int n_replies;

static void collect(void *ctx, const char *line) {
    (void)ctx;
    assert(n_replies < 8);
    snprintf(replies[n_replies++], sizeof(replies[0]), "%s", line);
}

static int dbm(uint8_t level) { return 22 - 2 * level; }

static bool starts(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

static void ask(api_t *api, const char *command) {
    char line[64];
    snprintf(line, sizeof(line), "%s", command);
    n_replies = 0;
    api_command(api, line, 10 * SEC, collect, NULL);
}

static const char *api_status_of(const api_t *api) {
    static char line[API_LINE];
    api_status(api, 10 * SEC, line, sizeof(line));
    return line;
}

static void test_api(void) {
    node_t n;
    uint8_t b[PKT_POSITION_LEN];
    group_key_t g;
    group_key_init(&g, KEY);
    node_init(&n, &CFG, 7);
    gnss_fix_t f = fix_at(1792670405000ll, true);
    node_on_fix(&n, &f, 0);
    node_on_packet(&n, b, packet(b, &g, 2, 1, 10, 64192), -71, 33, 3 * SEC);
    api_t api = {.node = &n, .board = "test", .reset = "poweron", .power = 3, .tx_dbm = dbm};

    ask(&api, "STATUS");
    assert(n_replies == 1 && starts(replies[0], "STATUS proto=5 board=test reset=poweron up=10 slot=1 slots=4 keyed=1 group=86 "));
    assert(strstr(replies[0], " stopped=0 help=0 heard=0 ") && strstr(replies[0], " riders=1 known=00000004 "));
    assert(strstr(replies[0], " power=3 dbm=16"));
    ask(&api, "riders"); /* the console types it in lower case */
    assert(n_replies == 2 && starts(replies[0], "RIDER slot=2 ") && strcmp(replies[1], "RIDERS 1") == 0);
    assert(strstr(replies[0], " gps=1 help=0 stopped=0 "));

    /* The key it has, sent again by a phone that reconnects: the riders stay. */
    ask(&api, "KEY 000102030405060708090a0b0c0d0e0f");
    assert(strcmp(replies[0], "OK keyed group=86") == 0);
    ask(&api, "RIDERS");
    assert(n_replies == 2);
    /* A new key: another group, so the riders heard under the old one go. */
    ask(&api, "KEY 000102030405060708090A0B0C0D0E0e");
    assert(starts(replies[0], "OK keyed group=") && api.setup_changed);
    ask(&api, "RIDERS");
    assert(n_replies == 1 && strcmp(replies[0], "RIDERS 0") == 0);
    ask(&api, "KEY 0001020304050607");
    assert(starts(replies[0], "ERR "));
    ask(&api, "KEY 000102030405060708090a0b0c0d0eXY");
    assert(starts(replies[0], "ERR "));

    ask(&api, "SLOT 5");
    assert(strcmp(replies[0], "OK slot=5") == 0 && n.cfg.slot == 5);
    ask(&api, "SLOT 32");
    assert(starts(replies[0], "ERR ") && n.cfg.slot == 5);
    ask(&api, "LEDGER");
    assert(n_replies == 1 && starts(replies[0], "LEDGER minutes=60 limit_ms=360000 used_ms=0 positions_ms=0,"));
    ask(&api, "HELP 1");
    assert(strcmp(replies[0], "OK help=1") == 0 && n.help);
    ask(&api, "HELP 2");
    assert(starts(replies[0], "ERR ") && n.help);
    ask(&api, "help 0");
    assert(strcmp(replies[0], "OK help=0") == 0 && !n.help);
    ask(&api, "SLOTS 5");
    assert(strcmp(replies[0], "OK slots=5") == 0 && n.cfg.slots == 5);
    ask(&api, "SLOTS 0");
    assert(starts(replies[0], "ERR ") && n.cfg.slots == 5);
    ask(&api, "SLOTS 33");
    assert(starts(replies[0], "ERR ") && n.cfg.slots == 5);
    ask(&api, "INTERVAL 499");
    assert(starts(replies[0], "ERR ") && n.cfg.interval_ms == 2000);
    ask(&api, "POWER 0");
    assert(strcmp(replies[0], "OK power=0 dbm=22") == 0 && api.power == 0 && api.radio_changed);
    api.radio_changed = false;
    ask(&api, "MODE 9 250");
    assert(strcmp(replies[0], "OK mode=9/250") == 0 && n.cfg.phy.sf == 9 && n.cfg.phy.bw_hz == 250000);
    assert(api.radio_changed && n.cfg.phy.freq_hz == 869525000);
    ask(&api, "MODE 10 62");
    assert(strcmp(replies[0], "OK mode=10/62") == 0 && n.cfg.phy.bw_hz == 62500);
    ask(&api, "MODE 6 250");
    assert(starts(replies[0], "ERR ") && n.cfg.phy.sf == 10);
    ask(&api, "MODE 9 300");
    assert(starts(replies[0], "ERR "));
    ask(&api, "MODE 9");
    assert(starts(replies[0], "ERR "));
    ask(&api, "MODE 9 250 x");
    assert(starts(replies[0], "ERR "));
    ask(&api, "STATUS");
    assert(strstr(replies[0], " mode=10/62 "));

    /* A group's whole setup in one command, taken whole or not at all: with a slot outside the
     * group, a short key, an interval too short, a part missing or one too many, nothing changes. */
    static const char *const partial[] = {
        "GROUP 000102030405060708090a0b0c0d0e0f 6 6 3000", "GROUP 000102030405060708090a0b0c0d0e 2 6 3000",
        "GROUP 000102030405060708090a0b0c0d0e0f 2 6 499", "GROUP 000102030405060708090a0b0c0d0e0f 2 6",
        "GROUP 000102030405060708090a0b0c0d0e0f 2 6 3000 1", "GROUP"};
    for (size_t i = 0; i < sizeof(partial) / sizeof(partial[0]); i++) {
        api.setup_changed = false;
        ask(&api, partial[i]);
        assert(n_replies == 1 && starts(replies[0], "ERR group") && !api.setup_changed);
        assert(n.cfg.slot == 5 && n.cfg.slots == 5 && n.cfg.interval_ms == 2000 && n.cfg.key[15] == 0x0e);
    }
    ask(&api, "GROUP 000102030405060708090a0b0c0d0e0f 2 6 3000");
    assert(n_replies == 1 && strcmp(replies[0], "OK group=86 slot=2 slots=6 interval=3000") == 0);
    assert(n.cfg.slot == 2 && n.cfg.slots == 6 && n.cfg.interval_ms == 3000 && n.cfg.key[15] == 0x0f);
    assert(api.setup_changed);
    /* A setup the board could not keep over a restart, its status says so. */
    assert(strstr(api_status_of(&api), " kept=1"));
    api.save_failed = true;
    assert(strstr(api_status_of(&api), " kept=0"));

    /* What only the caller can do is left to it, unanswered here. */
    ask(&api, "FORGET");
    assert(n_replies == 0 && api.forget);
    ask(&api, "REBOOT");
    assert(n_replies == 0 && api.restart);
    ask(&api, "FORGET now");
    assert(n_replies == 1 && starts(replies[0], "ERR "));
    ask(&api, "");
    assert(n_replies == 1 && starts(replies[0], "ERR "));

    /* The longest status there can be still fits a line. */
    n.sent = n.tx_late = n.tx_unsure = n.tx_skipped = n.rx_bad = n.rx_foreign = n.rx_forged = 4294967295u;
    n.rx_replay = n.rx_clash = 4294967295u;
    n.cfg.slot = 31;
    n.held.lat_e7 = -900000000;
    n.held.lon_e7 = -1800000000;
    n.lag_us = n.lag_min_us = n.lag_max_us = 999999;
    n.help_heard_by = 0xFFFFFFFFu;
    n.mark_err_us = 999999999;
    char line[API_LINE + 64];
    const int len = api_status(&api, 99999999 * SEC, line, sizeof(line));
    assert(len > 300 && len < API_LINE);
    /* And the longest rider line: each field at its widest, and every other rider answering. */
    n.riders[0] = (rider_t){.known = true, .fix_utc_ms = 1, .rssi_dbm = -32768, .snr_db4 = -32767,
                            .received = 4294967295u, .answered_by = 0xFFFFFFFEu,
                            .last = {.lat_e7 = -900000000, .lon_e7 = -1800000000, .speed_kmh = 254,
                                     .course2 = 179, .status = 0x0F, .call = 7, .counter = 65535}};
    const int rider_len = node_format_rider(&n, 0, 99999999 * SEC, line, sizeof(line));
    assert(rider_len > 250 && rider_len < API_LINE);
    printf("api: status, riders, the key, settings, and what it leaves to the caller\n");
}

int main(void) {
    test_airtime();
    test_crypto();
    test_codec();
    test_nmea();
    test_node_sends();
    test_node_time();
    test_node_duty();
    test_node_hears();
    test_node_help();
    test_api();
    return 0;
}
