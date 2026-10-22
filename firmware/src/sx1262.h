#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "phy.h"

/*
 * The SX1262, driven over SPI: from dmd2-lora-bridge at 2234b5a, with the radio mode a parameter
 * instead of EBYTE's fixed one, and the packet our own bytes instead of EBYTE's frame.
 *
 * Pins default to the Heltec WiFi LoRa 32 V4.3; another board names its own (a -D flag, or the
 * Zephyr build's board.h).
 */

/* Writes the mode and power to the chip and starts listening. True once the chip accepted every
 * command and reports it is receiving. Power levels are 0..3, 0 the strongest. */
void radio_init(void);
bool radio_apply(const phy_t *phy, uint8_t power_level);

/* Reads back, writing nothing: the chip still holds our sync word, and is listening. A chip that
 * reset itself comes back with its default sync word. */
bool radio_verify(void);

/* Sends one packet and waits for it to leave; then listens again. */
bool radio_send(const uint8_t *data, size_t len);

/* Waits at most wait_ms for a packet. Returns its length, with the modem's RSSI at the antenna in
 * dBm and its SNR in quarter dB; 0 when none came or it failed its CRC. */
size_t radio_receive(uint8_t *out, size_t cap, uint32_t wait_ms, int *rssi_dbm, int *snr_db4);

/* What a power level puts out on this board, in dBm. */
int radio_tx_dbm(uint8_t level);

#ifndef PIN_SX_NSS
#define PIN_SX_NSS 8
#endif
#ifndef PIN_SX_SCK
#define PIN_SX_SCK 9
#endif
#ifndef PIN_SX_MOSI
#define PIN_SX_MOSI 10
#endif
#ifndef PIN_SX_MISO
#define PIN_SX_MISO 11
#endif
#ifndef PIN_SX_RST
#define PIN_SX_RST 12
#endif
#ifndef PIN_SX_BUSY
#define PIN_SX_BUSY 13
#endif
#ifndef PIN_SX_DIO1
#define PIN_SX_DIO1 14
#endif

/*
 * Receive enable, on a board whose antenna switch takes one line for receive while DIO2 drives
 * the transmit side - Seeed's Wio-SX1262 (Wio Tracker L1), for one. High while listening, low
 * for transmitting. -1, the default, is a board without one.
 */
#ifndef PIN_SX_RXEN
#define PIN_SX_RXEN -1
#endif

/*
 * KCT8103L front end on the Heltec V4.3. It comes up unpowered, and that failure is invisible: SPI
 * answers, every command succeeds, and the radio is deaf and mute the whole time. All three pins
 * are needed. -DPIN_FEM_VCTRL=-1 is a board without a front end.
 */
#ifndef PIN_FEM_VCTRL
#define PIN_FEM_VCTRL 7   /* VFEM_Ctrl - supplies the FEM */
#endif
#ifndef PIN_FEM_CSD
#define PIN_FEM_CSD 2     /* PA_CSD - chip shutdown, high to enable */
#endif
#ifndef PIN_FEM_CTX
/* PA_CTX: with CSD high, low is receive through the LNA, high is transmit; CPS, wired to DIO2 on a
 * V4.3, then picks the amplifier over the bypass. */
#define PIN_FEM_CTX 5
#endif

/*
 * What the front end adds to a received signal before the modem measures it, taken off every
 * RSSI so a reading means the same on every board: the V4.3's KCT8103L LNA adds 21 dB (Meshtastic's
 * heltec_v4 variant). Unmeasured here.
 */
#ifndef SX_RX_GAIN_DB
#if PIN_FEM_VCTRL >= 0
#define SX_RX_GAIN_DB 21
#else
#define SX_RX_GAIN_DB 0
#endif
#endif

/* The TCXO, powered from DIO3: 1.8 V on these boards. Off-voltage, the carrier drifts without
 * failing anything loudly. */
#ifndef SX_TCXO_MV
#define SX_TCXO_MV 1800
#endif
#if SX_TCXO_MV == 1600
#define SX_TCXO_CODE 0x00
#elif SX_TCXO_MV == 1700
#define SX_TCXO_CODE 0x01
#elif SX_TCXO_MV == 1800
#define SX_TCXO_CODE 0x02
#elif SX_TCXO_MV == 2200
#define SX_TCXO_CODE 0x03
#elif SX_TCXO_MV == 2400
#define SX_TCXO_CODE 0x04
#elif SX_TCXO_MV == 2700
#define SX_TCXO_CODE 0x05
#elif SX_TCXO_MV == 3000
#define SX_TCXO_CODE 0x06
#elif SX_TCXO_MV == 3300
#define SX_TCXO_CODE 0x07
#else
#error "SX_TCXO_MV must be one of the voltages SetDIO3AsTcxoCtrl accepts"
#endif
