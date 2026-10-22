/*
 * LoRa Convoy on the Seeed Wio Tracker L1 Pro (Zephyr, nRF52840): the GNSS awake, the OLED dark,
 * storage opened and its bonds read, then the same firmware as every board (run.c). The storage
 * steps are dmd2-lora-bridge's, and its reasons go with them.
 */
#include "port.h"
#include "run.h"
#include "store.h"

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

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
    const char *reset = run_reset_name();
    const int watch = port_wdt_watch();
#ifdef PIN_GNSS_WAKE
    port_pin_output(PIN_GNSS_WAKE);
    port_pin_set(PIN_GNSS_WAKE, 1);
#endif
#ifdef BOARD_OLED_SH1106
    oled_off();
#endif

    /* Another firmware's file system - an L1 leaves the factory with Meshtastic or MeshCore - holds
     * no owner of ours to protect, so it is cleared without asking. */
    if (store_holds_littlefs()) {
        LOGW(TAG, "storage holds another firmware's file system - clearing it");
        store_erase();
    }
    int err = settings_subsys_init();
    if (err != 0) {
        char why[24];
        snprintf(why, sizeof(why), "settings %d", err);
        run_erase_when_asked(why, watch, store_erase);
        err = settings_subsys_init();
        if (err != 0) {
            LOGE(TAG, "storage still will not open (%d)", err);
            k_panic();
        }
    }
    /*
     * The bonds, read before anything loads settings: Zephyr's loader deletes every entry whose
     * name it cannot read, and a store with no owner would let any phone claim the board. An
     * unreadable store stays as it is, and Bluetooth off, until the button erases it.
     */
    static struct store_bonds bonds;
    err = store_scan_bonds(&bonds);
    if (err != 0) {
        char why[32];
        snprintf(why, sizeof(why), "bond store unreadable, %d", err);
        run_erase_when_asked(why, watch, store_erase);
        /* Settings are still mounted over what was just erased: start again, not write into it. */
        port_restart(RESTART_NOTE_MAGIC | RESTART_FOR_BOND);
    }
    run_start("wio_tracker_l1", reset);
    port_wdt_unwatch(watch);
    return 0;
}
