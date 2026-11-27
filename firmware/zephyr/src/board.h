#pragma once
/*
 * What sx1262.h and main.c need to know about this board, read from its devicetree rather than
 * written down twice. Included ahead of every source of the app (see CMakeLists.txt), because
 * sx1262.h's defaults are the Heltec's and apply to whatever has not been named first. From
 * dmd2-lora-bridge.
 *
 * Pins are port * 32 + pin, the number port_zephyr.c takes apart again.
 */
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/dt-bindings/lora/sx126x.h>

#define BOARD_LORA DT_ALIAS(lora0)
#define BOARD_PSEL(node, prop) \
    (DT_PROP(DT_GPIO_CTLR(node, prop), port) * 32 + DT_GPIO_PIN(node, prop))

#if !DT_NODE_HAS_COMPAT(BOARD_LORA, semtech_sx1262)
#error "this build needs an SX1262 behind the board's lora0 alias"
#endif

/* The bus and its chip select come from the devicetree, which port_zephyr.c reads directly; the
 * numbers here are for the startup log only. */
#define PIN_SX_NSS                                                                   \
    (DT_PROP(DT_SPI_DEV_CS_GPIOS_CTLR(BOARD_LORA), port) * 32 +                      \
     DT_SPI_DEV_CS_GPIOS_PIN(BOARD_LORA))
#define PIN_SX_SCK  (-1)
#define PIN_SX_MOSI (-1)
#define PIN_SX_MISO (-1)

#define PIN_SX_RST  BOARD_PSEL(BOARD_LORA, reset_gpios)
#define PIN_SX_BUSY BOARD_PSEL(BOARD_LORA, busy_gpios)
#define PIN_SX_DIO1 BOARD_PSEL(BOARD_LORA, dio1_gpios)
#if DT_NODE_HAS_PROP(BOARD_LORA, rx_enable_gpios)
#define PIN_SX_RXEN BOARD_PSEL(BOARD_LORA, rx_enable_gpios)
#endif

/* No KCT8103L or anything like it: the chip's own output. */
#define PIN_FEM_VCTRL (-1)

/* The driver sets both of these itself, so a board that wires them otherwise has to say so here
 * rather than be driven wrongly. */
#if !DT_PROP(BOARD_LORA, dio2_tx_enable)
#error "the SX1262 driver drives the antenna switch from DIO2"
#endif
#if DT_PROP(BOARD_LORA, dio3_tcxo_voltage) != SX126X_DIO3_TCXO_1V8
#error "the SX1262 driver powers the TCXO at 1.8 V from DIO3"
#endif
#define SX_TCXO_MV 1800

/*
 * The L1's GNSS, an L76K on uart0 (app.overlay takes the UART from Zephyr's GNSS driver). It runs
 * from power-up; its wake line, P1.09 in Seeed's pin map as Meshtastic's variant has it (D0 =
 * GPIO 41), is held high so it never sleeps. Zephyr's board file does not name it.
 */
#if DT_NODE_HAS_COMPAT(DT_ROOT, seeed_wio_tracker_l1)
#define BOARD_NAME "wio_tracker_l1"
#define PIN_GNSS_WAKE (1 * 32 + 9)
#endif

/*
 * The T-Echo's, from Meshtastic's variant (boards/lilygo/t_echo): the same L76K with its PPS on
 * P1.04, its reset on P1.05, low for over 100 ms resetting it and so held high, and P0.12, which
 * powers the board's peripherals and is raised before anything else.
 */
#if DT_NODE_HAS_COMPAT(DT_ROOT, lilygo_t_echo)
#define BOARD_NAME "t_echo"
#define PIN_POWER_EN (0 * 32 + 12)
#define PIN_GNSS_WAKE (1 * 32 + 2)
#define PIN_GNSS_RESET (1 * 32 + 5)
#define PIN_GNSS_PPS (1 * 32 + 4)
/* The e-paper's lines as t_echo.dts has them, let go as the board switches off; its touch pad, low
 * when touched, and its screen's light, high lit (Meshtastic's variant: BUTTON_TOUCH_ACTIVE_LOW,
 * PIN_EINK_EN "really just backlight power"). */
#define PIN_EPD_CS (0 * 32 + 30)
#define PIN_EPD_DC (0 * 32 + 28)
#define PIN_EPD_RES (0 * 32 + 2)
#define PIN_EPD_BUSY (0 * 32 + 3)
#define PIN_TOUCH (0 * 32 + 11)
#define PIN_BACKLIGHT (1 * 32 + 11)
/* Its clock ran 102-115 ppm off without a fix, the same in three holdovers (07, 2026-11-14), though
 * Meshtastic's board file says it has a 32 kHz crystal and the build asks for it. */
#define NODE_DRIFT_PPM 150
/* Its LoRa antenna a centimetre from its GNSS's: sending every 2 s, it lost satellites in minutes and
 * found none after a cold start in 15 (lab, 2026-11-22). */
#define NODE_GNSS_QUIET 1
#endif

/* The Wio Tracker L1 Pro's own, measured on one board, 2026-11-11 and 12 (docs/07): its L76K's RMC,
 * sending GGA and RMC only, comes 138 ms after its second and 8.7 ms more for each satellite used
 * (187 ms at 5, 275 at 15, by 5,600 packets heard on a board with PPS); its radio takes 7.1 ms to
 * start. ponytail: one board's numbers for every nRF build, a straight line tried to 15 satellites
 * only, and +-50 ms kept from the desk; another board names its own. */
#ifndef NODE_NMEA_LAG_US
#define NODE_NMEA_LAG_US 138000
#endif
#ifndef NODE_NMEA_LAG_SAT_US
#define NODE_NMEA_LAG_SAT_US 8700
#endif
#ifndef NODE_NMEA_ERR_US
#define NODE_NMEA_ERR_US 100000
#endif
#ifndef NODE_TX_LEAD_US
#define NODE_TX_LEAD_US 7100
#endif
