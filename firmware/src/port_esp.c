/* port.h on ESP-IDF: the parts that are more than a call through. From dmd2-lora-bridge. */
#include "port.h"

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs.h"

#define TAG "port"

void port_mutex_lock(port_mutex_t m) { xSemaphoreTake(m, portMAX_DELAY); }

bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio) {
    return xTaskCreate(fn, name, stack, NULL, prio, NULL) == pdPASS;
}

/* A task that stops feeding for CONFIG_ESP_TASK_WDT_TIMEOUT_S reboots the board
 * (CONFIG_ESP_TASK_WDT_PANIC). ESP-IDF knows its tasks by handle; the number is for Zephyr's sake. */
int port_wdt_watch(void) {
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "task watchdog unavailable: %s", esp_err_to_name(err));
    return 0;
}

void port_wdt_feed(int watch) {
    (void)watch;
    esp_task_wdt_reset();
}

/* In NVS, beside the Bluetooth bonds; main has opened it. */
bool port_setup_load(void *buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open("convoy", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    const esp_err_t err = nvs_get_blob(h, "setup", buf, &n);
    nvs_close(h);
    return err == ESP_OK && n == len;
}

bool port_setup_save(const void *buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open("convoy", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "setup", buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

#ifndef PIN_PAIR_BUTTON
#error "PIN_PAIR_BUTTON must name this board's runtime BOOT button"
#endif

/* Pulled up and configured by main; BOOT is held when it reads low. */
bool port_button_down(void) { return gpio_get_level((gpio_num_t)PIN_PAIR_BUTTON) == 0; }

/* RTC memory survives a software restart but not a power loss. */
static RTC_NOINIT_ATTR uint32_t s_restart_note;

void port_restart(uint32_t note) {
    s_restart_note = note;
    esp_restart();
}

uint32_t port_restart_note(void) {
    const uint32_t note = s_restart_note;
    s_restart_note = 0;
    return note;
}

const char *port_reset_cause(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_EXT: return "pin";
        case ESP_RST_SW: return "sw";
        default: return "other";
    }
}

/*
 * The console the host is actually on: an ESP32-S3's native USB, /dev/ttyACM*, read directly
 * rather than through stdin, which sits on UART0 (the bridge's finding on this board).
 */
void port_console_open(void) {
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usb);
}

int port_console_getc(uint32_t timeout_ms) {
    uint8_t c;
    return usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(timeout_ms)) > 0 ? c : -1;
}

#ifndef PIN_GNSS_TX
#error "PIN_GNSS_TX and PIN_GNSS_RX name the board's GNSS UART"
#endif
#define GNSS_UART UART_NUM_1

/* TX is the board's line towards the GNSS, RX the one from it. */
void port_gnss_open(void) {
    const uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GNSS_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GNSS_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GNSS_UART, PIN_GNSS_TX, PIN_GNSS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

int port_gnss_getc(uint32_t timeout_ms) {
    uint8_t c;
    return uart_read_bytes(GNSS_UART, &c, 1, pdMS_TO_TICKS(timeout_ms)) > 0 ? c : -1;
}

/* Truly random once the radio or Bluetooth runs, and from the bootloader's entropy before. */
uint32_t port_random(void) { return esp_random(); }

size_t port_device_id(uint8_t *out, size_t cap) {
    uint8_t mac[6];
    if (cap < sizeof(mac) || esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
    for (size_t i = 0; i < sizeof(mac); i++) out[i] = mac[i];
    return sizeof(mac);
}
