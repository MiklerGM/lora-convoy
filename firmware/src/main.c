/*
 * LoRa Convoy on the Heltec WiFi LoRa 32 V4.3 (ESP-IDF): the GNSS kit's power and control lines,
 * then the same firmware as every board (run.c).
 */
#include "port.h"
#include "run.h"

#define TAG "main"

void app_main(void) {
    /*
     * Heltec's GNSS kit, as Meshtastic's heltec_v4 variant wires it: a load switch on EN, active
     * low; RESET held high, since low for over 100 ms resets the L76K; STANDBY high, which keeps it
     * awake. Not yet run on a board: if the GNSS stays silent, VEXT (GPIO 36, active low) is the
     * next thing to try.
     */
#ifdef PIN_GNSS_EN
    port_pin_output(PIN_GNSS_EN);
    port_pin_set(PIN_GNSS_EN, 0);
#endif
#ifdef PIN_GNSS_RESET
    port_pin_output(PIN_GNSS_RESET);
    port_pin_set(PIN_GNSS_RESET, 1);
#endif
#ifdef PIN_GNSS_STANDBY
    port_pin_output(PIN_GNSS_STANDBY);
    port_pin_set(PIN_GNSS_STANDBY, 1);
#endif
    run_start("heltec_v4");
}
