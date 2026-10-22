/*
 * LoRa Convoy on the Seeed Wio Tracker L1 Pro (Zephyr, nRF52840): the GNSS awake and the OLED
 * dark, then the same firmware as every board (run.c).
 */
#include "port.h"
#include "run.h"

#define TAG "main"

#ifdef BOARD_OLED_SH1106
#include <zephyr/drivers/i2c.h>

/* Display off, then its charge pump off; 0x00 says commands follow. Sent on the bus directly:
 * Zephyr's display driver ends its init by turning the panel on, old image and all. */
static void oled_off(void) {
    static const struct i2c_dt_spec oled = I2C_DT_SPEC_GET(BOARD_OLED_SH1106);
    static const uint8_t off[] = {0x00, 0xAE, 0xAD, 0x8A};
    if (!i2c_is_ready_dt(&oled) || i2c_write_dt(&oled, off, sizeof(off)) != 0) {
        LOGW(TAG, "display did not take the off command");
    }
}
#endif

int main(void) {
#ifdef PIN_GNSS_WAKE
    port_pin_output(PIN_GNSS_WAKE);
    port_pin_set(PIN_GNSS_WAKE, 1);
#endif
#ifdef BOARD_OLED_SH1106
    oled_off();
#endif
    run_start("wio_tracker_l1");
    return 0;
}
