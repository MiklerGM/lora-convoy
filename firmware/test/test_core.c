/*
 * The node core on the host: airtime against tools/airtime.py, the packet against a fixed vector,
 * NMEA as an L76K sends it, and the node's rules for sending and receiving.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    assert(phy_valid(&p) && !phy_ldro(&p));
    p.sf = 11, p.bw_hz = 125000;
    assert(phy_ldro(&p)); /* 16.384 ms symbols */
    p.sf = 10;
    assert(!phy_ldro(&p));
    p.bw_hz = 200000;
    assert(!phy_valid(&p));
    printf("phy: airtime matches tools/airtime.py in %zu cases\n", sizeof(cases) / sizeof(cases[0]));
}

/* ---- codec ----------------------------------------------------------------------- */

static void test_codec(void) {
    /* The vector in docs/PROTOCOL.md: slot 3 of group 7, fix 2026-10-22 12:00:05 UTC. */
    static const uint8_t want[PKT_POSITION_LEN] = {0x00, 0x07, 0x03, 0x01, 0x23, 0x45, 0xFA, 0xC5, 0x43,
                                                   0x0B, 0x84, 0x06, 0xC5, 0x56, 0x28, 0x87, 0x04};
    const position_t p = {.group = 7, .slot = 3, .counter = 0x012345, .fix_time = 64197,
                          .lat_e7 = 471410000, .lon_e7 = 95215000, .speed_kmh = 40,
                          .course2 = 135, .status = ST_GPS_OK};
    uint8_t b[PKT_POSITION_LEN];
    assert(pkt_encode(b, &p) == PKT_POSITION_LEN && memcmp(b, want, sizeof(want)) == 0);

    position_t q;
    assert(pkt_decode(b, sizeof(b), &q));
    assert(q.group == 7 && q.slot == 3 && q.counter == 0x012345 && q.fix_time == 64197);
    assert(q.speed_kmh == 40 && q.course2 == 135 && q.status == ST_GPS_OK);
    /* half a step at most: 1.07e-5 degrees of latitude, 2.15e-5 of longitude */
    assert(labs((long)q.lat_e7 - 471410000) <= 54 && labs((long)q.lon_e7 - 95215000) <= 108);

    /* the corners of the world, and a value just below zero */
    static const int32_t lats[] = {900000000, -900000000, -1, 0};
    static const int32_t lons[] = {1800000000, -1800000000, -1, 0};
    for (size_t i = 0; i < 4; i++) {
        position_t c = p;
        c.lat_e7 = lats[i];
        c.lon_e7 = lons[i];
        pkt_encode(b, &c);
        assert(pkt_decode(b, sizeof(b), &q));
        assert(labs((long)q.lat_e7 - lats[i]) <= 108 && labs((long)q.lon_e7 - lons[i]) <= 215);
    }

    /* not a v0 position: another length, version or type */
    assert(!pkt_decode(b, sizeof(b) - 1, &q));
    b[0] = 1 << 6;
    assert(!pkt_decode(b, sizeof(b), &q));
    b[0] = 1 << 4;
    assert(!pkt_decode(b, sizeof(b), &q));
    printf("codec: the fixed vector, the world's corners, and refusals\n");
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

static const node_config_t CFG = {.group = 7, .slot = 1, .interval_ms = 2000, .duty_permille = 100,
                                  .phy = {869525000, 250000, 7, 5, 8, {0x64, 0x74}}};

static gnss_fix_t fix_at(int64_t utc_ms, bool valid) {
    return (gnss_fix_t){.time_valid = true, .valid = valid, .utc_ms = utc_ms, .lat_e7 = 471410000,
                        .lon_e7 = 95215000, .speed_kmh10 = 400, .course_deg10 = 2700, .sats = 9};
}

static void test_node_sends(void) {
    node_t n;
    uint8_t b[32];
    node_init(&n, &CFG, 12345, 0);
    /* Silent before the first fix, whatever the time. */
    for (int64_t t = 0; t < 10 * SEC; t += 100000) assert(node_poll_tx(&n, t, b, sizeof(b)) == 0);
    assert(n.tx_skipped >= 4);

    /* Then a packet every 1.6 to 2.4 s, each with the next counter. */
    const int64_t utc0 = 1792670405000ll;
    int64_t last = -1, t = 10 * SEC, shortest = 10 * SEC, longest = 0;
    int sent = 0;
    position_t p;
    for (; t < 70 * SEC; t += 10000) {
        if (t % SEC == 0) {
            gnss_fix_t f = fix_at(utc0 + (t - 10 * SEC) / 1000, true);
            node_on_fix(&n, &f, t);
        }
        const size_t len = node_poll_tx(&n, t, b, sizeof(b));
        if (!len) continue;
        assert(len == PKT_POSITION_LEN && pkt_decode(b, len, &p));
        assert(p.counter == (uint32_t)sent && p.slot == 1 && p.group == 7 && (p.status & ST_GPS_OK));
        if (last >= 0) {
            assert(t - last >= 1600000 - 10000 && t - last <= 2400000 + 10000);
            if (t - last < shortest) shortest = t - last;
            if (t - last > longest) longest = t - last;
        }
        last = t;
        sent++;
    }
    assert(sent >= 25 && sent <= 37);
    /* drawn anew each time, so two senders in step drift apart */
    assert(shortest < 1850000 && longest > 2150000);

    /* The fix is lost: the held position goes on, with its old time and without GPS_OK. */
    gnss_fix_t lost = fix_at(utc0 + 60500, false);
    node_on_fix(&n, &lost, t);
    for (; t < 80 * SEC; t += 10000) {
        if (node_poll_tx(&n, t, b, sizeof(b))) {
            assert(pkt_decode(b, PKT_POSITION_LEN, &p) && !(p.status & ST_GPS_OK));
            assert(p.fix_time == (uint16_t)((utc0 + 59000) / 1000)); /* the last valid one, at 69 s */
        }
    }
    printf("node: silent before a fix, then on its interval, and a lost fix sent as held\n");
}

static void test_node_duty(void) {
    /* 1 per mille is 3.6 s an hour: 138 packets of 26 ms, and then silence. */
    node_config_t cfg = CFG;
    cfg.duty_permille = 1;
    cfg.interval_ms = 1000;
    node_t n;
    uint8_t b[32];
    node_init(&n, &cfg, 1, 0);
    gnss_fix_t f = fix_at(1792670405000ll, true);
    int sent = 0;
    for (int64_t t = 0; t < 30 * 60 * SEC; t += 100000) {
        node_on_fix(&n, &f, t);
        sent += node_poll_tx(&n, t, b, sizeof(b)) > 0;
    }
    assert(sent == 138 && node_duty_used_ms(&n, 30 * 60 * SEC) == 138 * 26);
    /* The hour, to the minute: what was booked in minute 0 counts through minute 59, not 60. */
    node_t one;
    node_init(&one, &cfg, 1, 0);
    node_on_fix(&one, &f, 0);
    while (!node_poll_tx(&one, one.next_tx_us, b, sizeof(b))) {
    }
    assert(node_duty_used_ms(&one, 59 * 60 * SEC + 59 * SEC) == 26);
    assert(node_duty_used_ms(&one, 60 * 60 * SEC) == 0);
    /* An hour after the first packets the budget comes back. */
    int later = 0;
    for (int64_t t = 70 * 60 * SEC; t < 75 * 60 * SEC; t += 100000) {
        node_on_fix(&n, &f, t);
        later += node_poll_tx(&n, t, b, sizeof(b)) > 0;
    }
    assert(later > 100);
    printf("node: the duty cycle stops it at the limit, and the budget returns after the hour\n");
}

static size_t packet(uint8_t *b, uint8_t group, uint8_t slot, uint32_t counter, uint16_t fix_time) {
    const position_t p = {.group = group, .slot = slot, .counter = counter, .fix_time = fix_time,
                          .lat_e7 = 471500000, .lon_e7 = 95100000, .speed_kmh = 33, .course2 = 10,
                          .status = ST_GPS_OK};
    return pkt_encode(b, &p);
}

static void test_node_hears(void) {
    node_t n;
    uint8_t b[PKT_POSITION_LEN];
    char line[256];
    node_init(&n, &CFG, 7, 0);
    const int64_t utc0 = 1792670405000ll; /* 12:00:05, 64197 modulo 65536 */
    gnss_fix_t f = fix_at(utc0, true);
    node_on_fix(&n, &f, 0);

    /* Rider 2, its fix 5 s before ours, heard 3 s after our fix. */
    assert(node_on_packet(&n, b, packet(b, 7, 2, 10, 64192), -71, 33, 3 * SEC));
    node_format_rider(&n, 2, 3 * SEC, line, sizeof(line));
    assert(strstr(line, "fix_age=8 ") && strstr(line, "lat=47.15000") && strstr(line, "rssi=-71 "));
    assert(strstr(line, "snr=8.25 ") && strstr(line, "speed=33 course=20 gps=1"));

    /* Delivered share: counters 10, 11, 13 make three of four. */
    node_on_packet(&n, b, packet(b, 7, 2, 11, 64194), -70, 30, 5 * SEC);
    assert(!node_on_packet(&n, b, packet(b, 7, 2, 11, 64194), -70, 30, 5 * SEC)); /* again */
    node_on_packet(&n, b, packet(b, 7, 2, 13, 64198), -70, 30, 7 * SEC);
    node_format_rider(&n, 2, 7 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=3/4"));
    /* It restarted: its counter went back, and its run starts again. */
    node_on_packet(&n, b, packet(b, 7, 2, 0, 64200), -70, 30, 9 * SEC);
    node_format_rider(&n, 2, 9 * SEC, line, sizeof(line));
    assert(strstr(line, "rx=1/1"));

    /* A fix stamped in the future (its clock is wrong) shows as such, not as fresh. */
    node_on_packet(&n, b, packet(b, 7, 3, 0, 64197 + 20), -80, 0, 2 * SEC);
    node_format_rider(&n, 3, 2 * SEC, line, sizeof(line));
    assert(strstr(line, "fix_age=-18 "));
    /* 65000 s ago reads as 536 s ahead: past half its range the nearest wins (RFC 1982). */
    node_on_packet(&n, b, packet(b, 7, 4, 0, (uint16_t)(64197 - 65000)), -80, 0, 0);
    node_format_rider(&n, 4, 0, line, sizeof(line));
    assert(strstr(line, "fix_age=65000 ") == NULL && strstr(line, "fix_age=-536 "));

    /* Another group, our own slot, and noise are counted and kept out. */
    assert(!node_on_packet(&n, b, packet(b, 8, 5, 0, 64197), -80, 0, 0) && n.rx_foreign == 1);
    assert(!node_on_packet(&n, b, packet(b, 7, 1, 0, 64197), -80, 0, 0) && n.rx_clash == 1);
    assert(!node_on_packet(&n, b, 5, -80, 0, 0) && n.rx_bad == 1);
    assert(!n.riders[5].known && !n.riders[1].known);

    /* Without a time of our own, an age cannot be told: it says so. */
    node_t blind;
    node_init(&blind, &CFG, 7, 0);
    node_on_packet(&blind, b, packet(b, 7, 2, 0, 64192), -71, 33, 0);
    node_format_rider(&blind, 2, 0, line, sizeof(line));
    assert(strstr(line, "fix_age=? "));
    node_format_status(&n, 9 * SEC, line, sizeof(line));
    assert(strstr(line, "STATUS slot=1 group=7 fix=0 ") && strstr(line, "riders=3 "));
    printf("node: ages unwrapped and honest, the delivered share, and what it keeps out\n");
}

int main(void) {
    test_airtime();
    test_codec();
    test_nmea();
    test_node_sends();
    test_node_duty();
    test_node_hears();
    return 0;
}
