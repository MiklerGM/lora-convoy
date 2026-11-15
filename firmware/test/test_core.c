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
    /* The vector in docs/PROTOCOL.md, from an independent implementation on OpenSSL
     * (pyca/cryptography): slot 3, session 0x0102, sent at 2026-10-22 12:00:05 UTC, count 5, a
     * current fix 2 s old on PPS time, calling for help; one passenger, slot 7, stopped and
     * wanting a stop, as it said 12 minutes ago; under the key 00 01 .. 0f. */
    static const uint8_t want[PKT_LEN(1)] = {0x50, 0x86, 0x03, 0x01, 0x02, 0x6A, 0xD9, 0xFA, 0xC5, 0x05, 0x60, 0xE6,
                                             0x68, 0x00, 0x30, 0xF3, 0x70, 0x89, 0xEF, 0xBB, 0x87, 0x3F, 0xF3, 0x08,
                                             0x40, 0x9C, 0xC5, 0x4E, 0xC1, 0x8C, 0x76, 0x99, 0x1C, 0x4B};
    group_key_t g;
    group_key_init(&g, KEY);
    /* the group's id, which the phone tells groups apart by, its tag first: from OpenSSL */
    assert(g.tag == 0x86 && g.id == 0x86c91c92u);
    const packet_t p = {.session = 0x0102, .sent = 1792670405u, .count = 5,
                        .own = {.slot = 3, .has_pos = true, .lat_e7 = 471410000, .lon_e7 = 95215000, .age = 2,
                                .facts = FACT_CURRENT | FACT_TIME_PRECISE, .state = STATE_HELP},
                        .speed_kmh = 40, .course2 = 135, .beat = 3, .sats = 12, .passengers = 1,
                        .passenger = {{.slot = 7, .has_pos = true, .lat_e7 = 471500000, .lon_e7 = 95100000,
                                       .age = 130, .facts = FACT_STOPPED, .state = STATE_STOP_SOON}}};
    uint8_t b[PKT_MAX_LEN];
    assert(sizeof(want) == 34 && pkt_seal(b, &p, &g) == sizeof(want) && memcmp(b, want, sizeof(want)) == 0);

    packet_t q;
    assert(pkt_open(b, sizeof(want), &g, &q) == PKT_OK);
    assert(q.group == 0x86 && q.own.slot == 3 && q.session == 0x0102 && q.sent == 1792670405u && q.count == 5);
    assert(q.own.has_pos && q.own.age == 2 && q.own.facts == (FACT_CURRENT | FACT_TIME_PRECISE) && q.own.state == STATE_HELP);
    assert(q.speed_kmh == 40 && q.course2 == 135 && q.beat == 3 && q.sats == 12 && q.passengers == 1);
    assert(q.passenger[0].slot == 7 && q.passenger[0].has_pos && q.passenger[0].age == 130 &&
           q.passenger[0].facts == FACT_STOPPED && q.passenger[0].state == STATE_STOP_SOON);
    /* half a step at most: 1.07e-5 degrees of latitude, 2.15e-5 of longitude */
    assert(labs((long)q.own.lat_e7 - 471410000) <= 54 && labs((long)q.own.lon_e7 - 95215000) <= 108);
    assert(labs((long)q.passenger[0].lat_e7 - 471500000) <= 54 && labs((long)q.passenger[0].lon_e7 - 95100000) <= 108);

    /* An age in a byte: seconds below two minutes, whole minutes to two hours, then "older"; read
     * back as the least it can mean, so a record is never made younger than it is. */
    static const uint32_t ages[][3] = {{0, 0, 0}, {119, 119, 119}, {120, 120, 120}, {179, 120, 120}, {180, 121, 180},
                                       {7319, 239, 7260}, {7320, AGE_OLDER, 7320}, {4000000000u, AGE_OLDER, 7320}};
    for (size_t i = 0; i < sizeof(ages) / sizeof(ages[0]); i++) {
        assert(age_code(ages[i][0]) == ages[i][1] && age_seconds((uint8_t)ages[i][1]) == ages[i][2]);
    }

    /* No position at all is said, not sent as a place: a rider in trouble before its first fix. And
     * a packet is as long as its passengers, none to three. */
    for (uint8_t riders = 0; riders <= PKT_PASSENGERS_MAX; riders++) {
        packet_t c = p;
        c.own.has_pos = false;
        c.passengers = riders;
        c.passenger[2] = (record_t){.slot = 31, .state = STATE_HELP, .age = 9};
        const size_t len = pkt_seal(b, &c, &g);
        assert(len == 25u + 9u * riders && pkt_open(b, len, &g, &q) == PKT_OK && q.passengers == riders);
        assert(!q.own.has_pos && q.own.state == STATE_HELP);
        assert(riders < 3 || (q.passenger[2].slot == 31 && !q.passenger[2].has_pos && q.passenger[2].state == STATE_HELP));
        assert(pkt_open(b, len - 1, &g, &q) == PKT_MALFORMED);
    }

    /* the corners of the world, and a value just below zero */
    static const int32_t lats[] = {900000000, -900000000, -1, 0};
    static const int32_t lons[] = {1800000000, -1800000000, -1, 0};
    for (size_t i = 0; i < 4; i++) {
        packet_t c = p;
        c.own.lat_e7 = lats[i];
        c.own.lon_e7 = lons[i];
        const size_t len = pkt_seal(b, &c, &g);
        assert(pkt_open(b, len, &g, &q) == PKT_OK);
        assert(labs((long)q.own.lat_e7 - lats[i]) <= 108 && labs((long)q.own.lon_e7 - lons[i]) <= 215);
    }

    /* Every byte is covered: the clear header, the body and the tag. */
    for (size_t i = 0; i < sizeof(want); i++) {
        pkt_seal(b, &p, &g);
        b[i] ^= 0x20;
        const pkt_result_t r = pkt_open(b, sizeof(want), &g, &q);
        assert(i == 0 ? r == PKT_MALFORMED : i == 1 ? r == PKT_FOREIGN : r == PKT_FORGED);
    }
    /* Another group's key: its tag differs, and so would its authentication. */
    uint8_t other[16];
    memcpy(other, KEY, 16);
    other[15] ^= 1;
    group_key_t h;
    group_key_init(&h, other);
    pkt_seal(b, &p, &h);
    assert(h.tag != g.tag && pkt_open(b, sizeof(want), &g, &q) == PKT_FOREIGN);
    b[1] = g.tag; /* even wearing our tag */
    assert(pkt_open(b, sizeof(want), &g, &q) == PKT_FORGED);
    printf("codec: the fixed vector, ages, passengers, the world's corners, and every byte authenticated\n");
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

/* A radio that takes 8 ms to start is handed each packet 8 ms before its slot, so its RF begins on
 * the slot: against a twin with no start-up on the same clock, every packet 8 ms earlier, none
 * lost; and a TxDone at the slot's moment plus the airtime is told as no lead at all. */
static void test_tx_lead(void) {
    node_config_t slow = CFG;
    slow.tx_lead_us = 8000;
    node_t a, b;
    uint8_t buf[PKT_MAX_LEN];
    node_init(&a, &CFG, 12345);
    node_init(&b, &slow, 12345);
    int64_t at_a[40], at_b[40];
    int na = 0, nb = 0;
    for (int64_t t = T0; t < 70 * SEC; t += 1000) {
        tick(&a, t);
        tick(&b, t);
        /* the moment to hand over, exactly: a poll comes up to a millisecond after it */
        const int64_t due = node_next_tx_us(&b, t);
        if (node_poll_tx(&b, t, buf, sizeof(buf))) {
            at_b[nb++] = t;
            node_on_tx_done(&b, due + 8000 + phy_airtime_us(&b.cfg.phy, PKT_LEN(0)));
        }
        if (node_poll_tx(&a, t, buf, sizeof(buf))) at_a[na++] = t;
    }
    assert(na == 30 && nb == 30);
    for (int i = 0; i < 30; i++) assert(at_a[i] - at_b[i] == 8000);
    char line[512];
    node_format_status(&b, 70 * SEC, line, sizeof(line));
    assert(strstr(line, " txlead=0/0/0 "));
    printf("node: a slow radio is handed its packet early, by its start-up\n");
}

static void test_node_sends(void) {
    node_t n;
    uint8_t b[PKT_MAX_LEN];
    group_key_t g;
    group_key_init(&g, KEY);
    node_init(&n, &CFG, 12345);
    /* Silent before its GNSS gives a time: no slot can be told, nothing is counted. */
    for (int64_t t = 0; t < T0; t += 1000) assert(node_poll_tx(&n, t, b, sizeof(b)) == 0);
    assert(node_next_tx_us(&n, 0) == INT64_MAX && n.tx_skipped == 0);

    /* Then once a frame, from the frame after the time came, inside slot 1: a spacing on from the
     * frame's start plus its phase, which is under a spacing and differs from frame to frame. */
    packet_t p;
    int sent = 0;
    int64_t t = T0, last_k = -1, phase_min = SEC, phase_max = 0;
    for (; t < 70 * SEC; t += 1000) {
        tick(&n, t);
        const size_t len = node_poll_tx(&n, t, b, sizeof(b));
        if (!len) continue;
        assert(len == PKT_LEN(0) && pkt_open(b, len, &g, &p) == PKT_OK);
        assert(p.count == (uint8_t)sent && p.session == n.session && p.own.slot == 1 && (p.own.facts & FACT_CURRENT));
        /* it names the second of its slot, and its fix's age from there */
        assert(p.sent == (uint32_t)((UTC0_MS * 1000 + (t - T0)) / 1000000) && p.own.age <= 1);
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
    assert(node_poll_tx(&n, due, b, sizeof(b)) == PKT_LEN(0));
    /* E0: the radio's TxDone 1.5 ms after the slot's moment and the packet's airtime is RF that
     * started 1.5 ms late, as the status says. */
    const int64_t air = phy_airtime_us(&n.cfg.phy, PKT_LEN(0));
    char e0[512];
    node_on_tx_done(&n, due + air + 1500);
    node_format_status(&n, due, e0, sizeof(e0));
    assert(strstr(e0, " txlead=1500/1500/1500 "));
    /* And on the far side: a board on the same clock hears it end 100 ms after its slot's moment
     * and its airtime, so it started 100 ms late. */
    node_t rx = n;
    rx.cfg.slot = 2;
    /* the packet taken when a poll found it: a poll's time is no measure, and none is said ... */
    assert(node_on_packet(&rx, b, PKT_LEN(0), -80, 0, due + air + 103000));
    node_format_rider(&rx, 1, due + air + 103000, e0, sizeof(e0));
    assert(strstr(e0, " offset=?"));
    /* ... until its RxDone, as the radio's interrupt stamped it */
    node_on_rx_done(&rx, due + air + 100000);
    node_format_rider(&rx, 1, due + air + 103000, e0, sizeof(e0));
    assert(strstr(e0, " offset=100000/100000/100000 late=100000 "));
    t = due;

    /* The fix is lost: the held position goes on, with its true age and no longer "current". */
    t = (t / SEC + 1) * SEC;
    gnss_fix_t lost = fix_at(UTC0_MS + (t - T0) / 1000, false);
    node_on_fix(&n, &lost, t);
    const int64_t last_valid = UTC0_MS + (t - T0) / 1000 - 1000;
    for (const int64_t end = t + 10 * SEC; t < end; t += 1000) {
        if (node_poll_tx(&n, t, b, sizeof(b))) {
            assert(pkt_open(b, PKT_LEN(0), &g, &p) == PKT_OK && !(p.own.facts & FACT_CURRENT));
            assert(p.sent - age_seconds(p.own.age) == (uint32_t)(last_valid / 1000));
        }
    }

    /* The nonce never repeats: a clock stepped back, which could name a second again, draws a new
     * session. */
    const uint16_t before = n.session;
    while (!node_poll_tx(&n, t, b, sizeof(b))) t += 1000;
    assert(pkt_open(b, PKT_LEN(0), &g, &p) == PKT_OK && p.session == before);
    n.last_sent += 3600;
    while (!node_poll_tx(&n, t, b, sizeof(b))) t += 1000;
    assert(pkt_open(b, PKT_LEN(0), &g, &p) == PKT_OK && p.session != before);

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

    /* The lag told may grow with the satellites the fix used: 100 ms and 5 ms for each. A fix of 9
     * satellites 145 ms after its second, and one of 20 satellites 200 ms after, both set the clock
     * to the second they name. */
    node_config_t sky = cfg;
    sky.nmea_lag_sat_us = 5000;
    for (int sats = 9; sats <= 20; sats += 11) {
        node_t s;
        node_init(&s, &sky, 1);
        gnss_fix_t g = fix_at(UTC0_MS, true);
        g.sats = (uint8_t)sats;
        const int64_t came = 5000000 + 100000 + 5000 * sats;
        node_on_fix(&s, &g, came);
        assert(node_utc_us(&s, came, &utc, &err) && utc == UTC0_MS * 1000 + 100000 + 5000 * sats);
    }

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
    /* A board whose crystal is known to run worse is told so: the same edge, three times the
     * drift, and NMEA's time is taken a third of the way on, before the edge's bound lies. */
    node_t t;
    node_config_t loose = cfg;
    loose.drift_ppm = 150;
    node_init(&t, &loose, 1);
    node_on_pps(&t, 6000000);
    f = fix_at(UTC0_MS + 1000, true);
    node_on_fix(&t, &f, 6130000);
    assert(t.time == TIME_PPS && node_utc_us(&t, 66000000, &utc, &err) && err == 100 + 9000);
    f = fix_at(UTC0_MS + 140000, true);
    node_on_fix(&t, &f, 145130000);
    assert(t.time == TIME_NMEA);

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
    uint8_t b[PKT_MAX_LEN];
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

/* What rider slot would send in the second sent: its fix age seconds old, in session and count. */
static size_t packet(uint8_t *b, const group_key_t *g, uint8_t slot, uint16_t session, uint32_t sent, uint8_t count,
                     uint8_t age) {
    const packet_t p = {.session = session, .sent = sent, .count = count,
                        .own = {.slot = slot, .has_pos = true, .lat_e7 = 471500000, .lon_e7 = 95100000, .age = age,
                                .facts = FACT_CURRENT},
                        .speed_kmh = 33, .course2 = 10, .sats = 9};
    return pkt_seal(b, &p, g);
}

static void test_node_hears(void) {
    node_t n;
    uint8_t b[PKT_MAX_LEN];
    char line[400];
    group_key_t g, other;
    group_key_init(&g, KEY);
    uint8_t other_key[16] = {0};
    group_key_init(&other, other_key);
    node_init(&n, &CFG, 7);
    const int64_t utc0 = 1792670405000ll; /* 12:00:05 */
    const uint32_t s0 = (uint32_t)(utc0 / 1000);
    gnss_fix_t f = fix_at(utc0, true);
    node_on_fix(&n, &f, 0);

    /* Rider 2, heard 3 s after our fix, its own fix 5 s old then. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 3, 10, 5), -71, 33, 3 * SEC));
    node_format_rider(&n, 2, 3 * SEC, line, sizeof(line));
    assert(strstr(line, " pos=1 lat=47.15000") && strstr(line, " fix_age=5 ") && strstr(line, "rssi=-71 "));
    assert(strstr(line, "snr=8.25 ") && strstr(line, "speed=33 course=20 gps=1"));
    assert(strstr(line, " state=0 via=- direct=0.0 ") && strstr(line, " sats=9"));
    /* and three seconds on with nothing new, its fix is three seconds older */
    node_format_rider(&n, 2, 6 * SEC, line, sizeof(line));
    assert(strstr(line, " fix_age=8 ") && strstr(line, " heard=3.0 "));

    /* A held position beyond two minutes old comes with its age in whole minutes, rounded down: its
     * fix must not seem to creep forward and jump back with each of them. Rider 5 lost its fix at
     * s0 - 200; we never place it earlier than that, never later than we already knew, and within
     * a few minutes to the second or two. */
    uint32_t placed = s0;
    for (uint32_t at = 0; at < 200; at += 7) {
        packet_t held = {.session = 1, .sent = s0 + at, .count = (uint8_t)at,
                         .own = {.slot = 5, .has_pos = true, .lat_e7 = 1, .lon_e7 = 2, .age = age_code(200 + at)}};
        assert(node_on_packet(&n, b, pkt_seal(b, &held, &g), -70, 30, (int64_t)at * SEC));
        assert(n.riders[5].fix_utc <= placed && n.riders[5].fix_utc >= s0 - 200);
        placed = n.riders[5].fix_utc;
    }
    assert(placed <= s0 - 198);
    memset(&n.riders[5], 0, sizeof(n.riders[5]));

    /* Delivered share: counts 10, 11, 13 make three of four; 11 again is a repeat. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 5, 11, 0), -70, 30, 5 * SEC));
    assert(!node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 5, 11, 0), -70, 30, 5 * SEC) && n.rx_replay == 1);
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 7, 13, 0), -70, 30, 7 * SEC));
    node_format_rider(&n, 2, 7 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=3/4"));
    /* Two packets in one second are told apart by their counts, in order only. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 7, 14, 0), -70, 30, 7 * SEC));
    assert(!node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 7, 13, 0), -70, 30, 7 * SEC) && n.rx_replay == 2);
    /* It restarted: a new session, and its run starts again. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 200, s0 + 9, 0, 0), -70, 30, 9 * SEC));
    node_format_rider(&n, 2, 9 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=1/1"));
    /* The old session played back: it names an earlier second than the last we took. */
    assert(!node_on_packet(&n, b, packet(b, &g, 2, 100, s0 + 8, 15, 0), -70, 30, 9 * SEC) && n.rx_replay == 3);
    /* A recording of a rider we never heard, played ten seconds late: genuine, and refused by its
     * time alone, where v3 took any first packet of a rider. */
    assert(!node_on_packet(&n, b, packet(b, &g, 0, 7, s0 + 1, 0, 0), -70, 30, 11 * SEC) && n.rx_replay == 4);
    assert(!n.riders[0].known);

    /* Another group, a forgery, our own slot and noise are counted and kept out. */
    assert(!node_on_packet(&n, b, packet(b, &other, 3, 1, s0, 0, 0), -80, 0, 0) && n.rx_foreign == 1);
    packet(b, &g, 3, 1, s0, 0, 0);
    b[12] ^= 0x01;
    assert(!node_on_packet(&n, b, PKT_LEN(0), -80, 0, 0) && n.rx_forged == 1);
    assert(!node_on_packet(&n, b, packet(b, &g, 1, 1, s0, 0, 0), -80, 0, 0) && n.rx_clash == 1);
    assert(!node_on_packet(&n, b, 5, -80, 0, 0) && n.rx_bad == 1);
    assert(!n.riders[3].known && !n.riders[1].known);

    /* Without a time of our own, an age cannot be told: it says so ... */
    node_t blind;
    node_init(&blind, &CFG, 7);
    node_on_packet(&blind, b, packet(b, &g, 2, 1, s0, 0, 5), -71, 33, 0);
    node_format_rider(&blind, 2, 0, line, sizeof(line));
    assert(strstr(line, "fix_age=? "));
    /* ... until the phone gives it a calendar: its UTC at our clock's 0, 12:00:05, give or take
     * 250 ms. The fix of 12:00:00 is 5 s old then, with no new packet; the calendar is the
     * phone's, apart from any GNSS time, and no slot opens on it. */
    node_set_phone_time(&blind, 0, 1792670405000000ll, 250000);
    node_format_rider(&blind, 2, 0, line, sizeof(line));
    assert(strstr(line, " fix_age=5 "));
    node_format_status(&blind, 0, line, sizeof(line));
    assert(strstr(line, " time=none ") && strstr(line, " cal=phone/250 "));
    uint8_t out[PKT_MAX_LEN];
    assert(node_next_tx_us(&blind, 0) == INT64_MAX && node_poll_tx(&blind, 3 * SEC, out, sizeof(out)) == 0);
    /* Its GNSS comes, and says the phone was 2 s ahead: at our clock's 10 s it is 12:00:13, not
     * 12:00:15. The fix is told by the better calendar now: 13 s old. */
    gnss_fix_t late = fix_at(1792670413000ll, false);
    node_on_fix(&blind, &late, 10 * SEC);
    node_format_rider(&blind, 2, 10 * SEC, line, sizeof(line));
    assert(strstr(line, " fix_age=13 "));
    node_format_status(&blind, 10 * SEC, line, sizeof(line));
    assert(strstr(line, " time=nmea ") && strstr(line, " cal=nmea/"));
    node_format_status(&n, 9 * SEC, line, sizeof(line));
    assert(starts(line, "slot=1 slots=4 keyed=1 group=86c91c92 mode=7/250 fix=0 ") && strstr(line, " riders=1 "));
    assert(strstr(line, " time=nmea err=1.") && strstr(line, " budget=183.5 lag=? "));
    assert(strstr(line, " foreign=1 forged=1 replay=4 clash=1"));
    printf("node: ages honest, the delivered share, and forgeries, replays and strangers kept out\n");
}

/* Riders for a while: each fixes on the second and sends in its slot, heard at once by those
 * hears[listener][sender] says. */
static void ride(node_t *v[], int count, const bool hears[4][4], int64_t from, int64_t to) {
    uint8_t b[PKT_MAX_LEN];
    for (int64_t t = from; t < to; t += 1000) {
        for (int i = 0; i < count; i++) {
            tick(v[i], t);
            const size_t len = node_poll_tx(v[i], t, b, sizeof(b));
            for (int j = 0; len && j < count; j++) {
                if (j != i && hears[j][i]) node_on_packet(v[j], b, len, -70, 30, t);
            }
        }
    }
}

/* Passengers (12, 2026-11-14): a rider in the middle carries the two ends to each other. */
static void test_node_passengers(void) {
    uint8_t b[PKT_MAX_LEN];
    char line[400];
    packet_t p;
    group_key_t g;
    group_key_init(&g, KEY);
    node_t far, mid, end;
    node_t *v[] = {&far, &mid, &end};
    node_config_t c = CFG;
    c.passengers = 1;
    c.slot = 0;
    node_init(&far, &c, 1);
    c.slot = 1;
    node_init(&mid, &c, 2);
    c.slot = 2;
    node_init(&end, &c, 3);
    /* the middle hears both, and each end only the middle */
    static const bool BRIDGE[4][4] = {{0, 1, 0}, {1, 0, 1}, {0, 1, 0}};
    int64_t t = T0 + 20 * SEC;
    ride(v, 3, BRIDGE, T0, t);
    /* Each end has the other, told by the middle, and no older than the middle's turn allows: one
     * passenger a packet and two riders to carry is every second frame, so 6 s at most. */
    const uint32_t now_s = (uint32_t)((UTC0_MS * 1000 + (t - T0)) / 1000000);
    assert(end.riders[0].known && end.riders[0].via == 1 && !end.riders[0].heard);
    assert(far.riders[2].known && far.riders[2].via == 1 && now_s - far.riders[2].as_of <= 6);
    node_format_rider(&end, 0, t, line, sizeof(line));
    assert(strstr(line, " pos=1 lat=47.14100") && strstr(line, " via=1 direct=- ") && strstr(line, " speed=? course=? "));
    /* Each sees itself in the packets of those who hear it: its own packets get out. */
    assert(far.echo_by == 1u << 1 && end.echo_by == 1u << 1 && far.echo_news && mid.echo_by == (1u << 0 | 1u << 2));

    /* The far end calls for help. The middle's next three packets carry that, whoever's turn it
     * was, and the other end has it within two frames, told to its phone at once. */
    node_set_state(&far, STATE_HELP);
    int carried = 0;
    for (const int64_t until = t + 8 * SEC; t < until; t += 1000) {
        for (int i = 0; i < 3; i++) {
            tick(v[i], t);
            const size_t len = node_poll_tx(v[i], t, b, sizeof(b));
            for (int j = 0; len && j < 3; j++) {
                if (j != i && BRIDGE[j][i]) node_on_packet(v[j], b, len, -70, 30, t);
            }
            if (len && i == 1 && mid.riders[0].rec.state == STATE_HELP && carried < 3) {
                assert(pkt_open(b, len, &g, &p) == PKT_OK && p.passengers == 1);
                assert(p.passenger[0].slot == 0 && p.passenger[0].state == STATE_HELP);
                carried++;
                /* news for its phone of both: the middle itself and the caller it carried */
                assert(end.riders[0].rec.state == STATE_HELP && end.rx_changed == (1u << 1 | 1u << 0));
            }
        }
    }
    assert(carried == 3);
    /* The caller sees its call carried: the state it is in now, not the one before. */
    node_format_status(&far, t, line, sizeof(line));
    assert(strstr(line, " heard=1 state=1 echo=") && !strstr(line, " echo=- "));

    /* Only the state it is in now counts as carried: a rider still telling of it as it was before
     * the call must not tell its rider that the call is out. */
    node_t solo;
    node_init(&solo, &c, 4); /* slot 2 */
    gnss_fix_t f = fix_at(UTC0_MS, true);
    node_on_fix(&solo, &f, T0);
    node_set_state(&solo, STATE_HELP);
    packet_t tells = {.session = 1, .sent = (uint32_t)(UTC0_MS / 1000), .own = {.slot = 3}, .passengers = 1,
                      .passenger = {{.slot = 2, .state = STATE_NORMAL}}};
    assert(node_on_packet(&solo, b, pkt_seal(b, &tells, &g), -70, 30, T0) && solo.echo_by == 0 && !solo.echo_news);
    tells.count = 1;
    tells.passenger[0].state = STATE_HELP;
    assert(node_on_packet(&solo, b, pkt_seal(b, &tells, &g), -70, 30, T0) && solo.echo_by == 1u << 3 && solo.echo_news);

    /* A board deaf to a rider (the bench's DEAF) takes nothing from it directly, yet has it from
     * those who carry it: a chain on one desk. */
    node_t deaf;
    node_config_t cd = c;
    cd.slot = 5;
    cd.deaf = 1u << 0;
    node_init(&deaf, &cd, 6);
    node_on_fix(&deaf, &f, T0);
    const uint32_t s0 = (uint32_t)(UTC0_MS / 1000);
    packet_t direct = {.session = 1, .sent = s0, .own = {.slot = 0, .has_pos = true, .state = STATE_HELP}};
    packet_t told = {.session = 1, .sent = s0, .own = {.slot = 3, .has_pos = true}, .passengers = 1,
                     .passenger = {{.slot = 0, .has_pos = true, .age = 0, .state = STATE_HELP}}};
    assert(!node_on_packet(&deaf, b, pkt_seal(b, &direct, &g), -70, 30, T0) && !deaf.riders[0].known);
    assert(deaf.rx_replay == 0 && deaf.rx_forged == 0 && deaf.rx_bad == 0);
    assert(node_on_packet(&deaf, b, pkt_seal(b, &told, &g), -70, 30, T0));
    assert(deaf.riders[0].known && deaf.riders[0].via == 3 && !deaf.riders[0].heard &&
           deaf.riders[0].rec.state == STATE_HELP);

    /* A record never gets younger on its way: an older one of the caller, from a rider who met it
     * a minute ago, does not put the newer one out; a later one does. */
    const uint32_t s = (uint32_t)((UTC0_MS * 1000 + (t - T0)) / 1000000);
    packet_t old = {.session = 9, .sent = s, .count = 0, .own = {.slot = 3, .has_pos = true, .age = 0}, .passengers = 1,
                    .passenger = {{.slot = 0, .has_pos = true, .age = 60, .state = STATE_NORMAL}}};
    assert(node_on_packet(&end, b, pkt_seal(b, &old, &g), -70, 30, t));
    assert(end.riders[0].rec.state == STATE_HELP && end.riders[0].via == 1);
    old.count = 1;
    old.passenger[0].age = 0;
    old.passenger[0].state = STATE_STOP_NOW;
    old.sent = s + 2;
    assert(node_on_packet(&end, b, pkt_seal(b, &old, &g), -70, 30, t + 2 * SEC));
    assert(end.riders[0].rec.state == STATE_STOP_NOW && end.riders[0].via == 3);

    /* The far end goes out of reach. Its last record is carried on for ten minutes, growing older
     * honestly, and then no longer: a rider long gone takes no room from the present ones. */
    static const bool CUT[4][4] = {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}};
    ride(v, 3, CUT, t, t + 300 * SEC);
    t += 300 * SEC;
    int late_carried = 0;
    for (const int64_t until = t + 400 * SEC; t < until; t += 1000) {
        for (int i = 1; i < 3; i++) {
            tick(v[i], t);
            const size_t len = node_poll_tx(v[i], t, b, sizeof(b));
            if (!len) continue;
            node_on_packet(v[3 - i], b, len, -70, 30, t);
            assert(pkt_open(b, len, &g, &p) == PKT_OK);
            if (i == 1 && p.passengers && p.passenger[0].slot == 0) {
                /* as it said then: its age from that second, never from when it was passed on */
                const uint32_t said = mid.riders[0].as_of;
                assert(p.sent - age_seconds(p.passenger[0].age) - said < 60 && p.sent - said <= 600);
                late_carried++;
            }
        }
    }
    /* every third packet of the middle's, which has three riders to carry by now, for the five
     * minutes that were left of the ten */
    assert(late_carried > 40 && late_carried < 60 && mid.riders[0].known);
    printf("node: passengers carry the ends to each other, a changed state first, never younger, and not for ever\n");
}

/* The send rule (12, section 2): a board speaks when it has something to say. */
static void test_send_rule(void) {
    uint8_t b[PKT_MAX_LEN];
    node_t n;
    node_config_t c = CFG;
    c.beat = 3;
    c.move_m = 10;
    c.passengers = 1;
    node_init(&n, &c, 5);
    int64_t t = T0;
    int32_t north = 0;
    int course = 2700;
    /* runs for some frames at this place and heading; how many packets went */
    #define FRAMES(count) __extension__ ({ \
        int sent_ = 0; \
        for (const int64_t until_ = t + (count) * 2 * SEC; t < until_; t += 1000) { \
            if (t % SEC == 0) { \
                gnss_fix_t f_ = fix_at(UTC0_MS + (t - T0) / 1000, true); \
                f_.lat_e7 += north; \
                f_.course_deg10 = course; \
                node_on_fix(&n, &f_, t); \
            } \
            sent_ += node_poll_tx(&n, t, b, sizeof(b)) != 0; \
        } \
        sent_; })
    /* Standing: its first position, then one packet in eight frames; the rest of its slots pass in
     * silence, counted as such. */
    assert(FRAMES(1) == 1);
    assert(FRAMES(32) == 4 && n.tx_quiet == 28);
    /* on to just after its next heartbeat, which comes within eight frames */
    int waited = 0;
    while (FRAMES(1) == 0) assert(++waited < 8);
    /* 6 m is a GNSS's wander: nothing. 12 m is a move: the next slot says so, once. */
    north = 540;
    assert(FRAMES(3) == 0);
    north = 1080;
    assert(FRAMES(1) == 1 && FRAMES(3) == 0);
    /* A turn of 40 degrees, 6 m on. */
    north = 1620;
    course = 3100;
    assert(FRAMES(1) == 1 && FRAMES(3) == 0);
    /* A change of state goes out in three slots running, so that one loss does not hide it. */
    node_set_state(&n, STATE_STOP_SOON);
    assert(FRAMES(3) == 3 && FRAMES(2) == 0);
    /* And another rider's changed state, which it carries: three slots again. */
    group_key_t g;
    group_key_init(&g, KEY);
    packet_t other = {.session = 1, .sent = (uint32_t)((UTC0_MS * 1000 + (t - T0)) / 1000000), .own = {.slot = 3, .state = STATE_HELP}};
    assert(node_on_packet(&n, b, pkt_seal(b, &other, &g), -70, 30, t));
    assert(FRAMES(3) == 3 && FRAMES(2) == 0);
    #undef FRAMES
    printf("node: silent while nothing changes; a move, a turn, a state and a heartbeat each speak\n");
}

/* Time from the group (12, 2026-11-14): a board without PPS sets its clock by the packets of one
 * with it. Ten riders every 2 s leave 74 ms for a clock; the follower's NMEA is good to 100 ms and
 * comes 30 ms late, so alone it must stay silent. */
static void test_time_from_group(void) {
    uint8_t b[PKT_MAX_LEN];
    char line[600];
    packet_t p;
    group_key_t g;
    group_key_init(&g, KEY);
    node_config_t c = CFG;
    c.slots = 10;
    c.nmea_err_us = 100000;
    c.slot = 2;
    node_t lead, follower, third;
    node_init(&lead, &c, 1);
    c.slot = 1;
    node_init(&follower, &c, 2);
    c.slot = 3;
    node_init(&third, &c, 3);
    const int64_t air = phy_airtime_us(&c.phy, PKT_LEN(0));
    int64_t t = T0, worst = 0;
    bool lead_on = true;
    int lead_sent = 0;
    #define TRUE_UTC(at) (UTC0_MS * 1000 + ((at) - T0))
    #define RUN(seconds, step) \
        for (const int64_t until_ = t + (seconds) * SEC; t < until_; t += (step)) { \
            if (t % SEC == 0) node_on_pps(&lead, t); \
            if (t % SEC == 100000) { gnss_fix_t f_ = fix_at((TRUE_UTC(t) - 100000) / 1000, true); node_on_fix(&lead, &f_, t); } \
            if (t % SEC == 30000) { \
                gnss_fix_t f_ = fix_at((TRUE_UTC(t) - 30000) / 1000, true); \
                node_on_fix(&follower, &f_, t); \
                node_on_fix(&third, &f_, t); \
            } \
            size_t len_ = lead_on ? node_poll_tx(&lead, t, b, sizeof(b)) : 0; \
            if (len_) { \
                lead_sent++; \
                assert(pkt_open(b, len_, &g, &p) == PKT_OK && (p.own.facts & FACT_TIME_PRECISE)); \
                if (node_on_packet(&follower, b, len_, -70, 30, t + air)) node_on_rx_done(&follower, t + air); \
            } \
            len_ = node_poll_tx(&follower, t, b, sizeof(b)); \
            if (len_) { \
                /* it never calls its own time precise, and the third rider hears nobody else */ \
                assert(pkt_open(b, len_, &g, &p) == PKT_OK && !(p.own.facts & FACT_TIME_PRECISE)); \
                if (node_on_packet(&lead, b, len_, -70, 30, t + air)) node_on_rx_done(&lead, t + air); \
                if (node_on_packet(&third, b, len_, -70, 30, t + air)) node_on_rx_done(&third, t + air); \
            } \
            int64_t utc_, err_; \
            if (follower.time == TIME_GROUP && node_utc_us(&follower, t, &utc_, &err_) && llabs(utc_ - TRUE_UTC(t)) > worst) { \
                worst = llabs(utc_ - TRUE_UTC(t)); \
            } \
        }
    /* Before the leader's third packet nothing is set, and the follower keeps silent, unsure. */
    while (lead_sent < 2) { RUN(1, 250) }
    assert(follower.time == TIME_NMEA && follower.sent == 0 && follower.tx_unsure > 0);
    RUN(30, 250)
    /* Set: its clock within a millisecond of the truth it was 30 ms from, it says so, and sends. */
    int64_t utc, err;
    assert(follower.time == TIME_GROUP && node_utc_us(&follower, t, &utc, &err) && err >= 10000 && err < 10100);
    assert(worst < 1000 && follower.sent > 10);
    node_format_status(&follower, t, line, sizeof(line));
    assert(strstr(line, " time=group err=10.0 "));
    /* The leader, judging it by PPS, finds its packets in their slot to a millisecond; and is not
     * itself moved by anything it hears. */
    assert(lead.time == TIME_PPS && lead.riders[1].offset.n > 10);
    assert(lead.riders[1].offset.min_us > -1000 && lead.riders[1].offset.max_us < 1000);
    /* The third rider hears only the follower, whose time is borrowed: that sets nothing. */
    assert(third.riders[1].heard && third.time == TIME_NMEA && third.sent == 0);

    /* The leader falls silent. The follower goes on by its own clock, less sure by 3 ms a minute:
     * still sending after ten minutes ... */
    lead_on = false;
    const uint32_t sent_before = follower.sent;
    RUN(600, 1000)
    assert(follower.time == TIME_GROUP && node_utc_us(&follower, t, &utc, &err) && err > 39000 && err < 41000);
    assert(follower.sent > sent_before + 250);
    /* ... and back on its NMEA, and silent in this group, once that is the better of the two. */
    RUN(1300, 1000)
    assert(follower.time == TIME_NMEA);
    const uint32_t sent_after = follower.sent;
    RUN(20, 1000)
    assert(follower.sent == sent_after);
    #undef RUN
    #undef TRUE_UTC
    printf("node: a board without PPS set by one with it, to a millisecond; alone again, it drifts and says so\n");
}

static void test_node_mode(void) {
    /* D15: the mode in use is the mode applied. A node sending on v0's SF7/250 ... */
    node_t n;
    uint8_t b[PKT_MAX_LEN];
    char line[512];
    node_init(&n, &CFG, 21);
    int64_t t = T0;
    size_t len = 0;
    for (; !len; t += 1000) {
        tick(&n, t);
        len = node_poll_tx(&n, t, b, sizeof(b));
    }
    /* ... asked for SF11/250, as MODE asks: until the radio has taken it every slot passes over,
     * booking nothing, however many frames the radio takes, or if it never does. */
    phy_t slow = PHY_V0;
    slow.sf = 11;
    n.cfg.phy = slow;
    const uint32_t booked = node_duty_used_ms(&n, t);
    for (const int64_t end = t + 10 * SEC; t < end; t += 1000) {
        tick(&n, t);
        assert(node_poll_tx(&n, t, b, sizeof(b)) == 0);
    }
    assert(n.tx_held >= 4 && node_duty_used_ms(&n, t) == booked);
    /* Taken: its next slot sends, and books a packet of SF11's 370.688 ms. */
    node_phy_applied(&n, &slow);
    const int64_t due = node_next_tx_us(&n, t);
    for (; t < due; t += 1000) tick(&n, t);
    assert(node_poll_tx(&n, due, b, sizeof(b)) == PKT_LEN(0) && node_duty_used_ms(&n, due) == booked + 371);
    /* Its TxDone reckons with the airtime it went with, though v0's mode is asked for meanwhile:
     * RF 1.5 ms late, not 341 ms. */
    n.cfg.phy = PHY_V0;
    node_on_tx_done(&n, due + 370688 + 1500);
    node_format_status(&n, due, line, sizeof(line));
    assert(strstr(line, " txlead=1500/1500/1500 "));
    printf("node: a mode asked for is used once the radio has taken it, never before, and each packet "
           "keeps the airtime it went with\n");
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
    api_command(api, line, 10 * SEC, 10 * SEC, collect, NULL);
}

static const char *api_status_of(const api_t *api) {
    static char line[API_LINE];
    api_status(api, 10 * SEC, line, sizeof(line));
    return line;
}

/* TOUR: two boards told the same seconds are in the same mode at the same UTC with no word between
 * them: the three modes in turn, changed on the seconds of UTC that the dwell divides, and nothing
 * before a board has a time. */
static void test_tour(void) {
    node_t n;
    node_init(&n, &CFG, 1);
    api_t api = {.node = &n, .tour_s = 20};
    assert(!api_tour(&api, 0) && n.cfg.phy.sf == 7);
    int changes = 0, seen = 0;
    for (int64_t t = T0; t < T0 + 125 * SEC; t += SEC / 4) {
        tick(&n, t);
        const uint8_t before = n.cfg.phy.sf;
        if (!api_tour(&api, t)) continue;
        const int64_t utc_ms = UTC0_MS + (t - T0) / 1000;
        /* the first call puts it into the running dwell; every later change is on its boundary */
        assert(changes == 0 || utc_ms % 20000 == 0);
        assert(n.cfg.phy.sf != before || changes == 0);
        assert((n.cfg.phy.sf == 7 && n.cfg.phy.bw_hz == 250000) || (n.cfg.phy.sf == 9 && n.cfg.phy.bw_hz == 250000) ||
               (n.cfg.phy.sf == 10 && n.cfg.phy.bw_hz == 125000));
        seen |= 1 << (n.cfg.phy.sf - 7);
        changes++;
    }
    assert(changes >= 6 && seen == 0x0D);
    printf("api: a tour of the modes, by the clock\n");
}

static void test_api(void) {
    node_t n;
    uint8_t b[PKT_MAX_LEN];
    group_key_t g;
    group_key_init(&g, KEY);
    node_init(&n, &CFG, 7);
    gnss_fix_t f = fix_at(1792670405000ll, true);
    node_on_fix(&n, &f, 0);
    node_on_packet(&n, b, packet(b, &g, 2, 1, 1792670405u + 3, 10, 8), -71, 33, 3 * SEC);
    api_t api = {.node = &n, .board = "test", .reset = "poweron", .power = 3, .tx_dbm = dbm};

    ask(&api, "STATUS");
    /* each line says when it was made on the board's own clock, in ms: a moment a later GNSS
     * time, or a better reckoning of that clock against the phone's, leaves meaning the same */
    assert(n_replies == 1 && starts(replies[0], "STATUS proto=8 board=test reset=poweron at=10000 slot=1 "
                                                "slots=4 keyed=1 group=86c91c92 "));
    assert(strstr(replies[0], " stopped=0 heard=0 state=0 echo=- send=0/0/0 deaf=0 "));
    assert(strstr(replies[0], " power=3 dbm=16"));
    ask(&api, "riders"); /* the console types it in lower case */
    /* each rider line names the group it was heard in: one from before a new key is told apart */
    /* and a rider's line, the run it belongs to, the moment it was heard, and its fix's time as
     * the packet gave it: what the phone reckons the rider's ages from, again whenever it knows
     * the clocks better */
    char want[160];
    snprintf(want, sizeof(want), "RIDER slot=2 group=86c91c92 boot=%08lx at=10000 heard_at=3000 fix=64192 pos=1 ",
             (unsigned long)n.boot);
    assert(n_replies == 2 && starts(replies[0], want) && strcmp(replies[1], "RIDERS 1") == 0);
    assert(strstr(replies[0], " gps=1 help=0 stopped=0 "));
    /* A line that came at 9 s and waited for the node's lock while the rider was heard again, at
     * 9.5 s: its answer is told as of when it is made, so no rider is heard in the future, which
     * the phone could not read. Only CLOCK answers with when the line came. */
    assert(node_on_packet(&n, b, packet(b, &g, 2, 1, 1792670405u + 9, 11, 13), -71, 33, 9 * SEC + SEC / 2));
    char waited[16] = "RIDERS";
    n_replies = 0;
    api_command(&api, waited, 9 * SEC, 10 * SEC, collect, NULL);
    assert(n_replies == 2 && strstr(replies[0], " heard=0.5 "));
    snprintf(waited, sizeof(waited), "CLOCK 123456");
    n_replies = 0;
    api_command(&api, waited, 9 * SEC, 10 * SEC, collect, NULL);
    int64_t utc10;
    assert(node_utc_ms(&n, 10 * SEC, &utc10));
    snprintf(want, sizeof(want), "CLOCK t1=123456 boot=%08lx rx=9000000 tx=10000000 utc=%lld", (unsigned long)n.boot,
             (long long)utc10);
    assert(n_replies == 1 && strcmp(replies[0], want) == 0);

    /* The key it has, sent again by a phone that reconnects: the riders stay. */
    ask(&api, "KEY 000102030405060708090a0b0c0d0e0f");
    assert(strcmp(replies[0], "OK keyed group=86c91c92") == 0);
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
    /* The phone reckons our clock against its own: its request's moment comes back, with ours
     * when the request came and when the answer was made, the run they belong to, and our GNSS
     * time then, which the phone tells its riders' ages by rather than its own calendar ... */
    ask(&api, "CLOCK 123456");
    snprintf(want, sizeof(want), "CLOCK t1=123456 boot=%08lx rx=10000000 tx=10000000 utc=%lld", (unsigned long)n.boot,
             (long long)utc10);
    assert(n_replies == 1 && strcmp(replies[0], want) == 0);
    /* ... or ? on a board with no GNSS time yet */
    node_t bare;
    node_init(&bare, &CFG, 9);
    api_t bare_api = {.node = &bare, .board = "test", .reset = "poweron", .power = 3, .tx_dbm = dbm};
    ask(&bare_api, "CLOCK 1");
    snprintf(want, sizeof(want), "CLOCK t1=1 boot=%08lx rx=10000000 tx=10000000 utc=?", (unsigned long)bare.boot);
    assert(n_replies == 1 && strcmp(replies[0], want) == 0);
    /* a phone up 30 days: its moment, past what a long holds on our boards, comes back whole */
    ask(&api, "CLOCK 2592000000");
    snprintf(want, sizeof(want), "CLOCK t1=2592000000 boot=%08lx rx=10000000 tx=10000000 utc=%lld",
             (unsigned long)n.boot, (long long)utc10);
    assert(n_replies == 1 && strcmp(replies[0], want) == 0);
    ask(&api, "CLOCK soon");
    assert(starts(replies[0], "ERR "));
    /* ... and gives us its calendar, in our clock's terms, for the riders' ages */
    ask(&api, "TIME 10000 1792670415000 250");
    assert(strcmp(replies[0], "OK time=phone err=250") == 0 && n.phone_time && n.phone_at_us == 10 * SEC);
    ask(&api, "TIME 10000 1792670415000");
    assert(starts(replies[0], "ERR "));
    ask(&api, "LEDGER");
    assert(n_replies == 1 && starts(replies[0], "LEDGER minutes=60 limit_ms=360000 used_ms=0 positions_ms=0,"));
    ask(&api, "HELP 1");
    assert(strcmp(replies[0], "OK help=1") == 0 && n.state == STATE_HELP);
    ask(&api, "HELP 2");
    assert(starts(replies[0], "ERR ") && n.state == STATE_HELP);
    ask(&api, "help 0");
    assert(strcmp(replies[0], "OK help=0") == 0 && n.state == STATE_NORMAL);
    ask(&api, "STATE 3");
    assert(strcmp(replies[0], "OK state=3") == 0 && n.state == STATE_STOP_SOON);
    ask(&api, "STATE 16");
    assert(starts(replies[0], "ERR ") && n.state == STATE_STOP_SOON);
    ask(&api, "STATE 0");
    ask(&api, "DEAF 3,17");
    assert(strcmp(replies[0], "OK deaf=20008") == 0 && n.cfg.deaf == (1u << 3 | 1u << 17));
    ask(&api, "DEAF 32");
    assert(starts(replies[0], "ERR ") && n.cfg.deaf == (1u << 3 | 1u << 17));
    ask(&api, "DEAF 3,");
    assert(starts(replies[0], "ERR "));
    ask(&api, "DEAF -");
    assert(strcmp(replies[0], "OK deaf=0") == 0 && n.cfg.deaf == 0);
    /* more passengers than a packet holds would write past it */
    ask(&api, "PASSENGERS 4");
    assert(starts(replies[0], "ERR ") && n.cfg.passengers == 0);
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
    assert(n_replies == 1 && strcmp(replies[0], "OK group=86c91c92 slot=2 slots=6 interval=3000") == 0);
    assert(n.cfg.slot == 2 && n.cfg.slots == 6 && n.cfg.interval_ms == 3000 && n.cfg.key[15] == 0x0f);
    assert(api.setup_changed);
    /* A setup the board could not keep over a restart, its status says so. */
    assert(strstr(api_status_of(&api), " kept=1"));
    api.save_failed = true;
    assert(strstr(api_status_of(&api), " kept=0"));

    /* What only the caller can do is left to it, unanswered here. */
    ask(&api, "GNSS 0");
    assert(n_replies == 1 && strcmp(replies[0], "OK gnss=0") == 0 && api.gnss_ask == 1);
    /* SYSTEMS goes to the GNSS as it stands: nothing the L76K has no meaning for is passed on */
    ask(&api, "SYSTEMS 1");
    assert(strcmp(replies[0], "OK systems=1") == 0 && api.gnss_systems == 1);
    ask(&api, "SYSTEMS 9");
    assert(strncmp(replies[0], "ERR", 3) == 0 && api.gnss_systems == 1);
    ask(&api, "FORGET");
    assert(n_replies == 0 && api.forget);
    ask(&api, "REBOOT");
    assert(n_replies == 0 && api.restart);
    ask(&api, "FORGET now");
    assert(n_replies == 1 && starts(replies[0], "ERR "));
    ask(&api, "");
    assert(n_replies == 1 && starts(replies[0], "ERR "));

    /* The longest status there can be still fits a line. */
    n.sent = n.tx_late = n.tx_unsure = n.tx_skipped = n.tx_quiet = n.tx_held = n.rx_bad = n.rx_foreign = n.rx_forged = 4294967295u;
    n.rx_replay = n.rx_clash = 4294967295u;
    n.cfg.slot = 31;
    n.held.lat_e7 = -900000000;
    n.held.lon_e7 = -1800000000;
    n.lag_us = n.lag_min_us = n.lag_max_us = 999999;
    n.echo_by = 0xFFFFFFFFu;
    n.echo_us = 1;
    n.state = 15;
    n.cfg.move_m = 1000;
    n.cfg.deaf = 0xFFFFFFFFu;
    n.cfg.beat = 7;
    n.tx_lead = (spread_t){.min_us = -99999999, .max_us = 999999999, .sum_us = -99999999, .n = 1};
    n.mark_err_us = 999999999;
    char line[API_LINE + 64];
    const int len = api_status(&api, 99999999 * SEC, line, sizeof(line));
    /* whole, to its last field: a length alone would pass a line cut short inside */
    static const char tail[] = " replay=4294967295 clash=4294967295 power=0 dbm=22 kept=0";
    assert(len > 300 && len < API_LINE && strcmp(line + len - (int)strlen(tail), tail) == 0);
    /* And the longest rider line: each field at its widest. */
    n.riders[0] = (rider_t){.known = true, .heard = true, .via = 31, .as_of = 1, .rssi_dbm = -32768, .snr_db4 = -32767,
                            .received = 4294967295u, .expected = 4294967295u, .speed_kmh = 254, .course2 = 179, .sats = 15,
                            .offset = {.min_us = -99999999, .max_us = 999999999, .sum_us = -99999999, .n = 1}, .late_us = -99999999,
                            .rec = {.has_pos = true, .lat_e7 = -900000000, .lon_e7 = -1800000000, .facts = 0x0F,
                                    .state = 15}};
    const int rider_len = node_format_rider(&n, 0, 99999999 * SEC, line, sizeof(line));
    assert(rider_len > 250 && rider_len < API_LINE);
    printf("api: status, riders, the key, settings, and what it leaves to the caller\n");
}

int main(void) {
    test_airtime();
    test_crypto();
    test_codec();
    test_nmea();
    test_tx_lead();
    test_node_sends();
    test_node_time();
    test_node_duty();
    test_node_hears();
    test_node_passengers();
    test_send_rule();
    test_time_from_group();
    test_node_mode();
    test_tour();
    test_api();
    return 0;
}
