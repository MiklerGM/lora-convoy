#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A LoRa radio mode, as the protocol names one (docs/PROTOCOL.md). Explicit header and CRC on in
 * every mode of v0; implicit header and no CRC are BACKLOG T-length.
 */
typedef struct {
    uint32_t freq_hz;
    uint32_t bw_hz;      /* 62500, 125000, 250000 or 500000 */
    uint8_t sf;          /* 7..12 */
    uint8_t cr;          /* the coding rate's denominator: 5 for 4/5 ... 8 for 4/8 */
    uint16_t preamble;   /* symbols */
    uint8_t sync[2];     /* the SX1262's LoRa sync word registers, 0x0740 and 0x0741 */
} phy_t;

/* v0's one mode: SF7/250 at 869.525 MHz, the middle of 869.40-869.65 MHz. */
extern const phy_t PHY_V0;

bool phy_valid(const phy_t *p);

/* Microseconds per symbol: exact for the four bandwidths above. */
uint32_t phy_symbol_us(const phy_t *p);

/* Low data rate optimisation, on from a 16.38 ms symbol as Semtech recommends. */
bool phy_ldro(const phy_t *p);

/* Time on air of one packet of len bytes; the same formula as tools/airtime.py. */
uint32_t phy_airtime_us(const phy_t *p, size_t len);
