/*
 * LoRa Convoy on the Heltec WiFi LoRa 32 V4.3 (ESP-IDF): storage, the button and the GNSS kit's
 * power and control lines, then the same firmware as every board (run.c).
 */
#include "port.h"
#include "run.h"

#include "esp_task_wdt.h"
#include "nvs_flash.h"

#define TAG "main"

static void erase_nvs(void) { ESP_ERROR_CHECK(nvs_flash_erase()); }

void app_main(void) {
    const char *reset = run_reset_name();
    const int watch = port_wdt_watch();
    gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_PAIR_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&button);
    /* The bonds live in NVS: storage that will not open waits for the button (run.h). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        run_erase_when_asked(esp_err_to_name(err), watch, erase_nvs);
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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
    run_start("heltec_v4", reset);
    /* app_main returns and its task ends; a watched task that ends without leaving trips the
     * watchdog. */
    esp_task_wdt_delete(NULL);
}
