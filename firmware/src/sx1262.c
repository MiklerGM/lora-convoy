/*
 * The SX1262 over SPI, from dmd2-lora-bridge at 2234b5a. What changed: the radio mode is a phy_t
 * given at run time rather than EBYTE's measured one fixed at build time, and packets are our
 * bytes rather than an EBYTE frame. The bus discipline, the errata and the checks that the chip
 * accepted each command are the bridge's, proven on these boards; their reasons are its comments.
 */
#include "sx1262.h"
#include "port.h"

#include <string.h>

#define TAG "sx1262"

/* ---- chip constants ------------------------------------------------------ */

#define CMD_SET_STANDBY          0x80
#define CMD_SET_TX               0x83
#define CMD_SET_RX               0x82
#define CMD_SET_RF_FREQUENCY     0x86
#define CMD_SET_PACKET_TYPE      0x8A
#define CMD_SET_TX_PARAMS        0x8E
#define CMD_SET_PA_CONFIG        0x95
#define CMD_SET_BUFFER_BASE      0x8F
#define CMD_SET_MODULATION       0x8B
#define CMD_SET_PACKET_PARAMS    0x8C
#define CMD_SET_DIO_IRQ_PARAMS   0x08
#define CMD_GET_IRQ_STATUS       0x12
#define CMD_CLEAR_IRQ_STATUS     0x02
#define CMD_SET_DIO2_RF_SWITCH   0x9D
#define CMD_SET_DIO3_TCXO        0x97
#define CMD_CALIBRATE            0x89
#define CMD_CALIBRATE_IMAGE      0x98
#define CMD_SET_REGULATOR_MODE   0x96
#define CMD_WRITE_REGISTER       0x0D
#define CMD_READ_REGISTER        0x1D
#define CMD_WRITE_BUFFER         0x0E
#define CMD_READ_BUFFER          0x1E
#define CMD_GET_RX_BUFFER_STATUS 0x13
#define CMD_GET_PACKET_STATUS    0x14
#define CMD_GET_STATUS           0xC0
#define CMD_CLEAR_DEVICE_ERRORS  0x07
#define CMD_SET_RXTX_FALLBACK    0x93
#define CHIP_MODE_RX             0x5   /* status bits 6:4 */

#define REG_LORA_SYNC_MSB 0x0740
#define REG_OCP           0x08E7
#define REG_TX_MODULATION 0x0889
#define REG_TX_CLAMP      0x08D8
#define REG_IQ_CONFIG     0x0736

#define IRQ_TX_DONE      (1u << 0)
#define IRQ_RX_DONE      (1u << 1)
#define IRQ_HEADER_ERR   (1u << 5)
#define IRQ_CRC_ERR      (1u << 6)
#define IRQ_TIMEOUT      (1u << 9)

#define PACKET_TYPE_LORA 0x01
#define STANDBY_RC       0x00

/*
 * What each level sets the chip to, and what leaves the antenna for it. Not a linear dBm knob:
 * these are RadioLib's measured settings for each chip output (paOptimizedTable, radiolib-org/
 * power-tests), as the bridge adopted them.
 */
#if PIN_FEM_VCTRL >= 0
/* Heltec V4.3: the KCT8103L adds 13 dB to a drive of up to 13 dBm and less above it, topping out
 * near 28 dBm (Heltec's table, as in Meshtastic's LoRaFEMInterface.cpp). */
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {1, 6, 22},   /* chip 17 dBm -> 28 dBm */
    {2, 2, 21},   /* chip 11 dBm -> 24 dBm */
    {3, 1, 21},   /* chip  6 dBm -> 19 dBm */
    {1, 1, 14},   /* chip  1 dBm -> 14 dBm */
};
static const int8_t TX_DBM[4] = {28, 24, 19, 14};
#else
/* No front end, as on the Wio Tracker L1: the chip's own output. */
static const struct { uint8_t duty, hp_max; int8_t pa_val; } POWER_PA[4] = {
    {4, 7, 22},   /* 22 dBm */
    {3, 6, 22},   /* 20 dBm */
    {1, 6, 22},   /* 17 dBm */
    {1, 4, 20},   /* 14 dBm */
};
static const int8_t TX_DBM[4] = {22, 20, 17, 14};
#endif

/* ---- bus ----------------------------------------------------------------- */

static port_mutex_t s_lock;
static phy_t s_phy;
static uint8_t s_power;
static bool s_applied;
static bool s_in_rx;

#define LOCK_WAIT_MS 2000

static bool bus_lock(void) { return port_mutex_take(s_lock, LOCK_WAIT_MS); }
static void bus_unlock(void) { port_mutex_give(s_lock); }

/* Timed against the microsecond clock, never ticks: at 100 Hz a tick is 10 ms, and BUSY settles
 * in hundreds of microseconds. */
static void delay_us(uint32_t us) { port_delay_us(us); }

/* The chip raises BUSY while it digests a command; talking over it is ignored silently. */
static bool wait_busy(uint32_t timeout_ms) {
    int64_t deadline = port_uptime_us() + (int64_t)timeout_ms * 1000;
    while (port_pin_get(PIN_SX_BUSY) != 0) {
        if (port_uptime_us() > deadline) {
            LOGW(TAG, "BUSY stuck high for %ums", (unsigned)timeout_ms);
            return false;
        }
        delay_us(50);
    }
    return true;
}

/* Set when the chip reported a command error. Each transfer's status byte tells of the command
 * before it, so a refusal is heard one transfer late. */
static bool s_cmd_error;
static uint8_t s_miso[260];
static uint8_t s_last_opcode;

static bool xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!rx) {
        if (len > sizeof(s_miso)) return false;
        rx = s_miso;
    }
    if (!wait_busy(100)) return false;
    const bool sent = port_radio_spi_xfer(tx, rx, len);
    bool ok = sent;
    /* BUSY rises a moment after NSS: sampled only on the way in, the next command races it. */
    if (ok) {
        delay_us(1);
        ok = wait_busy(100);
    }
    /* Status bits 3:1 - 3 timeout, 4 processing error, 5 failure to execute. */
    if (sent && len > 1) {
        const uint8_t status = (uint8_t)((rx[1] >> 1) & 0x07);
        if (status >= 3 && status <= 5) {
            LOGW(TAG, "command %02X rejected, status %u", s_last_opcode, status);
            s_cmd_error = true;
        }
    }
    s_last_opcode = tx[0];
    return ok;
}

static bool cmd(uint8_t opcode, const uint8_t *args, size_t n) {
    uint8_t buf[16];
    if (n + 1 > sizeof(buf)) return false;
    buf[0] = opcode;
    if (n) memcpy(&buf[1], args, n);
    return xfer(buf, NULL, n + 1);
}

/* GetStatus: the chip's mode, or -1 when the bus did not answer. */
static int chip_mode(void) {
    uint8_t tx[2] = {CMD_GET_STATUS, 0x00}, rx[4] = {0};
    if (!xfer(tx, rx, 2)) return -1;
    return (rx[1] >> 4) & 0x07;
}

static bool cmd_read(uint8_t opcode, uint8_t *out, size_t n) {
    uint8_t tx[16] = {0}, rx[16] = {0};
    if (n + 2 > sizeof(tx)) return false;
    tx[0] = opcode;
    if (!xfer(tx, rx, n + 2)) return false;
    memcpy(out, &rx[2], n);   /* opcode byte, then one status byte, then the payload */
    return true;
}

static bool reg_write(uint16_t addr, const uint8_t *data, size_t n) {
    uint8_t buf[8];
    if (n + 3 > sizeof(buf)) return false;
    buf[0] = CMD_WRITE_REGISTER;
    buf[1] = (uint8_t)(addr >> 8);
    buf[2] = (uint8_t)addr;
    memcpy(&buf[3], data, n);
    return xfer(buf, NULL, n + 3);
}

static bool reg_read(uint16_t addr, uint8_t *out, size_t n) {
    uint8_t tx[8] = {0}, rx[8] = {0};
    if (n + 4 > sizeof(tx)) return false;
    tx[0] = CMD_READ_REGISTER;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    if (!xfer(tx, rx, n + 4)) return false;
    memcpy(out, &rx[4], n);   /* opcode, address high, address low, one status byte */
    return true;
}

/* Errata 15.1: bit 2 of TxModulation cleared at 500 kHz and set at every other bandwidth, before
 * each transmission, since SetPacketParams puts it back. */
static bool fix_modulation_quality(void) {
    uint8_t v = s_phy.bw_hz == 500000 ? 0x00 : 0x04;
    return reg_write(REG_TX_MODULATION, &v, 1);
}

/* Errata 15.2: the PA clamp, at the value RadioLib leaves on this board. */
static bool fix_pa_clamping(void) {
    uint8_t v = 0xDE;
    return reg_write(REG_TX_CLAMP, &v, 1);
}

/* Errata 15.4, standard IQ. */
static bool fix_iq_polarity(void) {
    uint8_t v = 0x0D;
    return reg_write(REG_IQ_CONFIG, &v, 1);
}

/* 140 mA, the SX1262's high-power PA limit; SetPaConfig resets it, so it follows. */
static bool set_ocp(void) {
    uint8_t v = 0x38;
    return reg_write(REG_OCP, &v, 1);
}

static bool buffer_write(const uint8_t *data, size_t n) {
    uint8_t buf[2 + 256];
    if (n > 255) return false;
    buf[0] = CMD_WRITE_BUFFER;
    buf[1] = 0x00;
    memcpy(&buf[2], data, n);
    return xfer(buf, NULL, n + 2);
}

static bool buffer_read(uint8_t offset, uint8_t *out, size_t n) {
    uint8_t tx[3 + 256] = {0}, rx[260] = {0};
    if (n > 255) return false;
    tx[0] = CMD_READ_BUFFER;
    tx[1] = offset;
    if (!xfer(tx, rx, n + 3)) return false;
    memcpy(out, &rx[3], n);
    return true;
}

/* ---- front end ----------------------------------------------------------- */

static void fem_power(bool on) {
#if PIN_FEM_VCTRL >= 0
    /* The LDO's output ramps through 10 uF before any control pin may be driven. */
    port_pin_set(PIN_FEM_VCTRL, on);
    if (on) delay_us(10000);
    port_pin_set(PIN_FEM_CSD, on);
#else
    (void)on;
#endif
}

static void fem_transmit(bool tx) {
#if PIN_SX_RXEN >= 0
    port_pin_set(PIN_SX_RXEN, !tx);
#endif
#if PIN_FEM_VCTRL >= 0
    port_pin_set(PIN_FEM_CTX, tx);
    delay_us(1000);
#else
    (void)tx;
#endif
}

/* ---- configuration ------------------------------------------------------- */

static bool set_standby(void) {
    uint8_t a = STANDBY_RC;
    s_in_rx = false;
    return cmd(CMD_SET_STANDBY, &a, 1);
}

static bool set_frequency(uint32_t hz) {
    uint32_t frf = (uint32_t)(((uint64_t)hz << 25) / 32000000ull);
    uint8_t a[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    return cmd(CMD_SET_RF_FREQUENCY, a, 4);
}

static uint8_t bw_code(uint32_t bw_hz) {
    switch (bw_hz) {
        case 62500: return 0x03;
        case 125000: return 0x04;
        case 250000: return 0x05;
        default: return 0x06; /* 500 kHz */
    }
}

static bool set_modulation(void) {
    uint8_t a[4] = {s_phy.sf, bw_code(s_phy.bw_hz), (uint8_t)(s_phy.cr - 4), phy_ldro(&s_phy) ? 1 : 0};
    return cmd(CMD_SET_MODULATION, a, 4);
}

static bool set_packet_params(uint8_t payload_len) {
    uint8_t a[6] = {
        (uint8_t)(s_phy.preamble >> 8), (uint8_t)s_phy.preamble,
        0x00,          /* explicit header */
        payload_len,   /* to send, or the most the receiver accepts */
        0x01,          /* CRC on */
        0x00,          /* IQ normal */
    };
    return cmd(CMD_SET_PACKET_PARAMS, a, 6);
}

static bool set_power(uint8_t level) {
    const uint8_t i = level & 0x03;
    uint8_t pa[4] = {POWER_PA[i].duty, POWER_PA[i].hp_max, 0x00 /* SX1262 */, 0x01};
    if (!cmd(CMD_SET_PA_CONFIG, pa, 4)) return false;
    uint8_t tx[2] = {(uint8_t)POWER_PA[i].pa_val, 0x04 /* 200 us ramp */};
    return cmd(CMD_SET_TX_PARAMS, tx, 2);
}

/* Image calibration per band, datasheet table 9-2. */
static void image_band(uint32_t hz, uint8_t out[2]) {
    if (hz > 900000000) {
        out[0] = 0xE1, out[1] = 0xE9;   /* 902-928 MHz */
    } else if (hz > 850000000) {
        out[0] = 0xD7, out[1] = 0xDB;   /* 863-870 MHz */
    } else if (hz > 770000000) {
        out[0] = 0xC1, out[1] = 0xC5;   /* 779-787 MHz */
    } else if (hz > 460000000) {
        out[0] = 0x75, out[1] = 0x81;   /* 470-510 MHz */
    } else {
        out[0] = 0x6B, out[1] = 0x6F;   /* 430-440 MHz */
    }
}

static bool configure(void) {
    uint8_t reg = 0x01;                      /* DC-DC, as these boards are wired */
    uint8_t tcxo[4] = {SX_TCXO_CODE, 0x00, 0x01, 0x40};   /* 5 ms startup */
    uint8_t pkt = PACKET_TYPE_LORA;
    uint8_t base[2] = {0x00, 0x00};
    uint8_t rfsw = 0x01;
    uint8_t fallback = 0x20;   /* fall back to STDBY_RC, not FS */
    uint8_t calib = 0x7F;
    uint8_t image[2];
    image_band(s_phy.freq_hz, image);
    uint8_t irq[8] = {0xFF, 0xFF, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};

#define STEP(what, expr) do { if (!(expr)) { LOGW(TAG, "configure: %s failed", what); \
                                             return false; } } while (0)
    STEP("standby", set_standby());
    /* Cleared after the first command: its status byte describes a chip this sequence had not
     * touched yet, maybe still running another firmware's configuration. */
    s_cmd_error = false;
    STEP("regulator", cmd(CMD_SET_REGULATOR_MODE, &reg, 1));
    STEP("tcxo", cmd(CMD_SET_DIO3_TCXO, tcxo, 4));
    STEP("calibrate", cmd(CMD_CALIBRATE, &calib, 1));
    delay_us(5000);
    STEP("calibrate image", cmd(CMD_CALIBRATE_IMAGE, image, 2));
    /* After the calibrations. On a V4.3 DIO2 drives the front end's CPS, which picks the amplifier
     * over the bypass: lose this write and every transmission leaves about 28 dB down. */
    STEP("dio2 rf switch", cmd(CMD_SET_DIO2_RF_SWITCH, &rfsw, 1));
    STEP("packet type", cmd(CMD_SET_PACKET_TYPE, &pkt, 1));
    STEP("frequency", set_frequency(s_phy.freq_hz));
    STEP("modulation", set_modulation());
    STEP("errata 15.1", fix_modulation_quality());
    STEP("errata 15.2", fix_pa_clamping());
    STEP("errata 15.4", fix_iq_polarity());
    STEP("fallback mode", cmd(CMD_SET_RXTX_FALLBACK, &fallback, 1));
    STEP("packet params", set_packet_params(255));
    STEP("sync word", reg_write(REG_LORA_SYNC_MSB, s_phy.sync, 2));
    STEP("buffer base", cmd(CMD_SET_BUFFER_BASE, base, 2));
    STEP("power", set_power(s_power));
    STEP("ocp", set_ocp());
    /* Everything latched, only RxDone and TxDone on DIO1. */
    STEP("irq params", cmd(CMD_SET_DIO_IRQ_PARAMS, irq, 8));
    /* The last command is heard from only by asking. */
    STEP("status", chip_mode() >= 0);
    STEP("no command was rejected", !s_cmd_error);
#undef STEP
    return true;
}

static bool enter_rx(void) {
    uint8_t clear[2] = {0xFF, 0xFF};
    uint8_t rx[3] = {0xFF, 0xFF, 0xFF};   /* continuous */
    /* The payload length back to the maximum: a transmission leaves the receiver capped at what
     * it just sent. */
    if (!set_packet_params(255)) return false;
    s_cmd_error = false;
    if (!cmd(CMD_CLEAR_IRQ_STATUS, clear, 2)) return false;
    fem_transmit(false);
    if (!cmd(CMD_SET_RX, rx, 3)) return false;
    /* Listening once the chip says so, not once the bus took the command. */
    if (chip_mode() != CHIP_MODE_RX || s_cmd_error) return false;
    s_in_rx = true;
    return true;
}

/* ---- the interface ---------------------------------------------------------- */

void radio_init(void) {
    s_lock = port_mutex_create();
    static const int outputs[] = {
        PIN_SX_RST,
#if PIN_FEM_VCTRL >= 0
        PIN_FEM_VCTRL, PIN_FEM_CSD, PIN_FEM_CTX,
#endif
#if PIN_SX_RXEN >= 0
        PIN_SX_RXEN,
#endif
    };
    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) port_pin_output(outputs[i]);
    port_pin_input(PIN_SX_BUSY);
    port_pin_input(PIN_SX_DIO1);
    fem_power(true);
    fem_transmit(false);
    port_radio_spi_init(PIN_SX_NSS, PIN_SX_SCK, PIN_SX_MOSI, PIN_SX_MISO);
    port_pin_set(PIN_SX_RST, 0);
    delay_us(2000);
    port_pin_set(PIN_SX_RST, 1);
    port_sleep_ms(20);
    LOGI(TAG, "spi up (nss=%d busy=%d dio1=%d), front end %s", PIN_SX_NSS, PIN_SX_BUSY, PIN_SX_DIO1,
         PIN_FEM_VCTRL >= 0 ? "KCT8103L" : PIN_SX_RXEN >= 0 ? "none, RX enable" : "none");
}

bool radio_apply(const phy_t *phy, uint8_t power_level) {
    if (!phy_supported(phy) || power_level > 3 || !bus_lock()) return false;
    /* Forgotten before the first write: a sequence that stops partway leaves the chip partly on the
     * new mode, and radio_verify must not pass it. */
    s_applied = false;
    s_phy = *phy;
    s_power = power_level;
    const bool ok = configure() && enter_rx();
    s_applied = ok;
    bus_unlock();
    LOGI(TAG, "apply %lu Hz SF%u BW%lu CR4/%u power %d dBm -> %s", (unsigned long)phy->freq_hz,
         phy->sf, (unsigned long)(phy->bw_hz / 1000), phy->cr, TX_DBM[power_level & 3],
         ok ? "ok" : "FAILED");
    return ok;
}

bool radio_verify(void) {
    if (!bus_lock()) return false;
    uint8_t sync[2] = {0, 0};
    const bool answered = reg_read(REG_LORA_SYNC_MSB, sync, 2);
    const bool listening = s_in_rx;
    bus_unlock();
    if (!answered || !s_applied) return false;
    if (memcmp(sync, s_phy.sync, 2) != 0) {
        LOGW(TAG, "verify: sync word is %02X%02X, expected %02X%02X", sync[0], sync[1], s_phy.sync[0],
             s_phy.sync[1]);
        return false;
    }
    if (!listening) LOGW(TAG, "verify: not receiving");
    return listening;
}

bool radio_send(const uint8_t *data, size_t len) {
    if (len == 0 || len > 255 || !s_applied || !bus_lock()) return false;
    uint8_t clear[2] = {0xFF, 0xFF};
    uint8_t txbase[2] = {0x00, 0x00};
    uint8_t go[3] = {0x00, 0x00, 0x00};   /* no timeout */
    bool ok = set_standby() && set_packet_params((uint8_t)len) && cmd(CMD_SET_BUFFER_BASE, txbase, 2) &&
              buffer_write(data, len) && cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
    if (ok) {
        uint8_t noerr[2] = {0x00, 0x00};
        cmd(CMD_CLEAR_DEVICE_ERRORS, noerr, 2);
        fix_modulation_quality();
        fem_transmit(true);
        ok = cmd(CMD_SET_TX, go, 3);
        /* The packet's own airtime and half a second of slack, whatever the mode. */
        const int64_t deadline = port_uptime_us() + phy_airtime_us(&s_phy, len) + 500 * 1000;
        while (ok) {
            uint8_t irq[2] = {0, 0};
            if (!cmd_read(CMD_GET_IRQ_STATUS, irq, 2)) { ok = false; break; }
            const uint16_t flags = (uint16_t)((uint16_t)irq[0] << 8 | irq[1]);
            if (flags & IRQ_TX_DONE) break;
            if ((flags & IRQ_TIMEOUT) || port_uptime_us() > deadline) { ok = false; break; }
            delay_us(1000);
        }
        fem_transmit(false);
    }
    if (!ok) LOGW(TAG, "send: %u bytes did not complete", (unsigned)len);
    /* Back to listening whatever happened. */
    enter_rx();
    bus_unlock();
    return ok;
}

size_t radio_receive(uint8_t *out, size_t cap, uint32_t wait_ms, int *rssi_dbm, int *snr_db4) {
    if (!s_applied || !bus_lock()) return 0;
    if (!s_in_rx) enter_rx();
    size_t produced = 0;
    const int64_t deadline = port_uptime_us() + (int64_t)wait_ms * 1000;
    while (port_uptime_us() < deadline) {
        uint8_t irq[2] = {0, 0};
        if (!cmd_read(CMD_GET_IRQ_STATUS, irq, 2)) break;
        const uint16_t flags = (uint16_t)((uint16_t)irq[0] << 8 | irq[1]);
        if ((flags & IRQ_TIMEOUT) && !(flags & IRQ_RX_DONE)) {
            /* A symbol timeout leaves RX even in continuous mode; the flag follows the chip. */
            s_in_rx = false;
            if (!enter_rx()) break;
            continue;
        }
        if (!(flags & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR))) {
            /* Blocking, not spinning: the modem latches its flags, so a slower poll loses nothing. */
            bus_unlock();
            port_sleep_ms(5);
            if (!bus_lock()) return 0;
            continue;
        }
        uint8_t clear[2] = {0xFF, 0xFF};
        if (!(flags & (IRQ_CRC_ERR | IRQ_HEADER_ERR))) {
            uint8_t st[2] = {0, 0};
            if (cmd_read(CMD_GET_RX_BUFFER_STATUS, st, 2)) {
                size_t n = st[0] < cap ? st[0] : cap;
                uint8_t ps[3] = {0, 0, 0};
                if (n && buffer_read(st[1], out, n) && cmd_read(CMD_GET_PACKET_STATUS, ps, 3)) {
                    /* RSSI in half dBm, less what the front end added; SNR in quarter dB, signed */
                    *rssi_dbm = -(int)ps[0] / 2 - SX_RX_GAIN_DB;
                    *snr_db4 = (int)(int8_t)ps[1];
                    produced = n;
                }
            }
        }
        cmd(CMD_CLEAR_IRQ_STATUS, clear, 2);
        break;
    }
    bus_unlock();
    return produced;
}

int radio_tx_dbm(uint8_t level) { return TX_DBM[level & 3]; }
