#include "nmea.h"

#include <string.h>

void nmea_init(nmea_t *n) { memset(n, 0, sizeof(*n)); }

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "123.45" as an integer times 10^decimals, rounded down past them. Empty or malformed: false. */
static bool fixed(const char *s, int decimals, int64_t *out) {
    if (!*s) return false;
    int64_t v = 0;
    int seen = -1;
    bool digits = false;
    for (; *s; s++) {
        if (*s == '.') {
            if (seen >= 0) return false;
            seen = 0;
        } else if (*s >= '0' && *s <= '9') {
            digits = true;
            if (seen < 0) {
                v = v * 10 + (*s - '0');
            } else if (seen < decimals) {
                v = v * 10 + (*s - '0');
                seen++;
            }
        } else {
            return false;
        }
        if (v > 100000000000000ll) return false;
    }
    for (int i = seen < 0 ? 0 : seen; i < decimals; i++) v *= 10;
    *out = v;
    return digits;
}

/* "ddmm.mmmm" or "dddmm.mmmm" with its hemisphere, as degrees x 1e7. */
static bool coordinate(const char *s, const char *hemi, int max_deg, int32_t *out) {
    int64_t mm5; /* degrees and minutes, ddmm x 1e5 */
    if (!fixed(s, 5, &mm5)) return false;
    const int64_t deg = mm5 / 10000000;
    const int64_t min5 = mm5 % 10000000; /* minutes x 1e5 */
    if (deg > max_deg || min5 >= 6000000) return false;
    /* minutes x 1e5 to degrees x 1e7: x 100 / 60, rounded */
    int64_t e7 = deg * 10000000 + (min5 * 100 + 30) / 60;
    if (e7 > (int64_t)max_deg * 10000000) return false;
    if (*hemi == 'S' || *hemi == 'W') {
        e7 = -e7;
    } else if (*hemi != 'N' && *hemi != 'E') {
        return false;
    }
    *out = (int32_t)e7;
    return true;
}

/* Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "hhmmss.sss" and "ddmmyy" as Unix milliseconds. */
static bool utc(const char *time, const char *date, int64_t *out) {
    int64_t t, d;
    if (strlen(date) != 6 || !fixed(time, 3, &t) || !fixed(date, 0, &d)) return false;
    const int64_t hh = t / 10000000, mm = t / 100000 % 100, ss = t / 1000 % 100, ms = t % 1000;
    const unsigned day = (unsigned)(d / 10000), mon = (unsigned)(d / 100 % 100);
    const int64_t year = 2000 + d % 100;
    if (hh > 23 || mm > 59 || ss > 60 || day < 1 || day > 31 || mon < 1 || mon > 12) return false;
    *out = (days_from_civil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss) * 1000 + ms;
    return true;
}

#define MAX_FIELDS 20

/* Splits the sentence body in place; field 0 is the address, "GNRMC". */
static int split(char *s, char *f[MAX_FIELDS]) {
    int n = 0;
    f[n++] = s;
    for (; *s && n < MAX_FIELDS; s++) {
        if (*s == ',') {
            *s = '\0';
            f[n++] = s + 1;
        }
    }
    return n;
}

static bool sentence(nmea_t *n, gnss_fix_t *fix) {
    char *line = n->line;
    /* $<body>*<checksum> */
    char *star = strrchr(line, '*');
    if (line[0] != '$' || !star || strlen(star) != 3) return false;
    const int hi = hex(star[1]), lo = hex(star[2]);
    uint8_t sum = 0;
    for (char *p = line + 1; p < star; p++) sum ^= (uint8_t)*p;
    if (hi < 0 || lo < 0 || sum != (hi << 4 | lo)) {
        n->bad++;
        return false;
    }
    *star = '\0';
    char *f[MAX_FIELDS];
    const int count = split(line + 1, f);
    if (strlen(f[0]) != 5) return false;
    const char *type = f[0] + 2; /* past the talker */

    if (strcmp(type, "GGA") == 0 && count >= 9) {
        int64_t sats, hdop;
        n->sats = fixed(f[7], 0, &sats) && sats < 256 ? (uint8_t)sats : 0;
        n->hdop10 = fixed(f[8], 1, &hdop) && hdop < 65536 ? (uint16_t)hdop : 0;
        return false;
    }
    if (strcmp(type, "RMC") != 0 || count < 10) return false;

    gnss_fix_t out = {.speed_kmh10 = -1, .course_deg10 = -1, .sats = n->sats, .hdop10 = n->hdop10};
    out.time_valid = utc(f[1], f[9], &out.utc_ms);
    /* Mode, from NMEA 2.3 on: N is "not valid" even when the status says A. */
    const bool mode_ok = count < 13 || f[12][0] != 'N';
    if (f[2][0] == 'A' && mode_ok && out.time_valid) {
        out.valid = coordinate(f[3], f[4], 90, &out.lat_e7) && coordinate(f[5], f[6], 180, &out.lon_e7);
        if (!out.valid) n->bad++;
        int64_t v;
        if (fixed(f[7], 3, &v)) out.speed_kmh10 = (int32_t)((v * 1852 + 50000) / 100000); /* knots */
        if (fixed(f[8], 1, &v) && v < 3600) out.course_deg10 = (int32_t)v;
    }
    *fix = out;
    return true;
}

bool nmea_feed(nmea_t *n, char c, gnss_fix_t *fix) {
    if (c == '$') {
        n->len = 0;
        n->overflow = false;
    }
    if (c == '\r' || c == '\n') {
        bool done = false;
        if (n->len > 0 && !n->overflow) {
            n->line[n->len] = '\0';
            done = sentence(n, fix);
        }
        n->len = 0;
        n->overflow = false;
        return done;
    }
    if (n->len + 1 >= sizeof(n->line)) {
        n->overflow = true;
        return false;
    }
    n->line[n->len++] = c;
    return false;
}
