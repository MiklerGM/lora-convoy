#include "phy.h"

/*
 * The sync word is our own: registers 0x64 0x74 put sync symbols 48 and 56 on the air (the chip
 * takes each from its register's top five bits, times four; dmd2-lora-bridge PHY-notes). Public
 * LoRaWAN sends 24 and 32, the private default 8 and 16, Meshtastic 16 and 88, EBYTE 40 and 36.
 * Provisional until checked on hardware (BACKLOG T-phy).
 */
const phy_t PHY_V0 = {
    .freq_hz = 869525000,
    .bw_hz = 250000,
    .sf = 7,
    .cr = 5,
    .preamble = 8,
    .sync = {0x64, 0x74},
};

bool phy_valid(const phy_t *p) {
    const bool bw = p->bw_hz == 62500 || p->bw_hz == 125000 || p->bw_hz == 250000 ||
                    p->bw_hz == 500000;
    return bw && p->sf >= 7 && p->sf <= 12 && p->cr >= 5 && p->cr <= 8 && p->preamble >= 6 &&
           p->freq_hz >= 150000000 && p->freq_hz <= 960000000;
}

uint32_t phy_symbol_us(const phy_t *p) { return (uint32_t)((1000000ull << p->sf) / p->bw_hz); }

bool phy_ldro(const phy_t *p) { return phy_symbol_us(p) >= 16380; }

uint32_t phy_airtime_us(const phy_t *p, size_t len) {
    const int32_t sf = p->sf, de = phy_ldro(p) ? 1 : 0;
    const int32_t bits = 8 * (int32_t)len - 4 * sf + 28 + 16; /* explicit header, CRC on */
    const int32_t per = 4 * (sf - 2 * de);
    const int32_t blocks = bits > 0 ? (bits + per - 1) / per : 0;
    const uint32_t symbols4 = 4u * p->preamble + 17u            /* preamble + 4.25 */
                              + 4u * (8u + (uint32_t)blocks * p->cr);
    return (uint32_t)((uint64_t)symbols4 * phy_symbol_us(p) / 4u);
}
