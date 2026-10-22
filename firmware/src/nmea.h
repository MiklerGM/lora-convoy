#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the board's GNSS says, read from its NMEA sentences: RMC for time, position, speed and
 * course; GGA for satellites and HDOP. Any talker (GP, GN, GL, GA, BD, GB), as an L76K sends GN
 * for its multi-system fix.
 */
typedef struct {
    bool time_valid;      /* date and time read, with or without a position */
    bool valid;           /* a position fix: RMC status A, and a mode other than N */
    int64_t utc_ms;       /* Unix time of the fix, when time_valid */
    int32_t lat_e7;       /* degrees x 1e7 */
    int32_t lon_e7;
    int32_t speed_kmh10;  /* km/h x 10, -1 when not given */
    int32_t course_deg10; /* degrees x 10 from north, -1 when not given */
    uint8_t sats;         /* from the last GGA, 0 when unknown */
    uint16_t hdop10;      /* HDOP x 10 from the last GGA, 0 when unknown */
} gnss_fix_t;

typedef struct {
    char line[100];
    size_t len;
    bool overflow;
    uint8_t sats;
    uint16_t hdop10;
    uint32_t bad;          /* sentences dropped for a wrong checksum or a malformed field */
} nmea_t;

void nmea_init(nmea_t *n);

/* One byte from the GNSS. True when it completed an RMC, which is then in *fix. */
bool nmea_feed(nmea_t *n, char c, gnss_fix_t *fix);
