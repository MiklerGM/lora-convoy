/* port.h on ESP-IDF: the parts that are more than a call through. From dmd2-lora-bridge. */
#include "port.h"

#include <string.h>

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

#ifdef PIN_GNSS_PPS
/* The PPS edge, stamped in its interrupt: the clock read is all it does. */
static portMUX_TYPE s_pps_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_pps_us;

static void IRAM_ATTR on_pps(void *arg) {
    (void)arg;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_pps_lock);
    s_pps_us = now;
    portEXIT_CRITICAL_ISR(&s_pps_lock);
}
#endif

int64_t port_pps_us(void) {
#ifdef PIN_GNSS_PPS
    portENTER_CRITICAL(&s_pps_lock);
    const int64_t t = s_pps_us;
    portEXIT_CRITICAL(&s_pps_lock);
    return t;
#else
    return 0;
#endif
}

/* The radio's DIO1, which rises at its TxDone and RxDone (sx1262.c), stamped the same way: E0's
 * measure of when a packet left and came. Its error is the interrupt's latency, a microsecond's
 * clock plus however long the handler waits to run: to be measured (E0). */
static portMUX_TYPE s_dio1_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_dio1_us;

static void IRAM_ATTR on_dio1(void *arg) {
    (void)arg;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_dio1_lock);
    s_dio1_us = now;
    portEXIT_CRITICAL_ISR(&s_dio1_lock);
}

void port_radio_irq_open(int pin) {
    const gpio_config_t dio1 = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&dio1));
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err); /* installed already: fine */
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)pin, on_dio1, NULL));
}

int64_t port_radio_irq_us(void) {
    portENTER_CRITICAL(&s_dio1_lock);
    const int64_t t = s_dio1_us;
    portEXIT_CRITICAL(&s_dio1_lock);
    return t;
}

/* TX is the board's line towards the GNSS, RX the one from it. PPS, where wired, rises on the
 * second. */
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
#ifdef PIN_GNSS_PPS
    const gpio_config_t pps = {.pin_bit_mask = 1ULL << PIN_GNSS_PPS, .mode = GPIO_MODE_INPUT,
                               .intr_type = GPIO_INTR_POSEDGE};
    ESP_ERROR_CHECK(gpio_config(&pps));
    const esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err); /* installed already: fine */
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_GNSS_PPS, on_pps, NULL));
#endif
}

void port_gnss_write(const char *s) { uart_write_bytes(GNSS_UART, s, strlen(s)); }

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

/* ---- the screen and the battery: the Heltec V4.3's, as Meshtastic's heltec_v4 variant wires them -- */

#include "driver/i2c_master.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_lcd_panel_vendor.h"

#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_ADC_CTRL 37   /* high connects the battery's divider */
#define BATTERY_CHANNEL ADC_CHANNEL_0 /* GPIO 1 */

static esp_lcd_panel_handle_t s_panel;

/* Once, on the first picture: a screen that will not start leaves the board running without one. */
static bool screen_open(void) {
    static bool tried;
    if (tried) return s_panel != NULL;
    tried = true;
    port_pin_output(PIN_VEXT);
    port_pin_set(PIN_VEXT, 0);
    port_sleep_ms(50);
    i2c_master_bus_config_t bus_cfg = {.i2c_port = -1, .sda_io_num = PIN_OLED_SDA, .scl_io_num = PIN_OLED_SCL,
                                       .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                       .flags.enable_internal_pullup = true};
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = {.dev_addr = 0x3C, .control_phase_bytes = 1, .dc_bit_offset = 6,
                                            .lcd_cmd_bits = 8, .lcd_param_bits = 8, .scl_speed_hz = 400000};
    esp_lcd_panel_ssd1306_config_t ssd = {.height = 64};
    esp_lcd_panel_dev_config_t dev_cfg = {.bits_per_pixel = 1, .reset_gpio_num = PIN_OLED_RST, .vendor_config = &ssd};
    esp_lcd_panel_handle_t panel;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err == ESP_OK) err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err == ESP_OK) err = esp_lcd_new_panel_ssd1306(io, &dev_cfg, &panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    /* turned round, as the Heltec's case holds it [owner, 2026-11-25] */
    if (err == ESP_OK) err = esp_lcd_panel_mirror(panel, true, true);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "screen: %s", esp_err_to_name(err));
        return false;
    }
    s_panel = panel;
    return true;
}

bool port_screen_size(int *w, int *h) {
    *w = 128;
    *h = 64;
    return true;
}

/* ESP-IDF's SSD1306 takes the pages as screen.c draws them. */
void port_screen_show(const uint8_t *buf) {
    if (screen_open()) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 128, 64, buf);
}

/* The SSD1306's own display off: its charge pump stops and its memory keeps the picture. Not Vext,
 * which powers the radio's front end too (Meshtastic's heltec_v4 variant). */
/* No VBUS sense on the Heltec V4.3 that Meshtastic's variant names; the voltage tells where it can. */
bool port_usb_power(void) { return false; }

bool port_screen_light(bool on) { return screen_open() && esp_lcd_panel_disp_on_off(s_panel, on) == ESP_OK; }

int port_battery_mv(void) {
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static bool tried;
    if (!tried) {
        tried = true;
        port_pin_output(PIN_ADC_CTRL);
        adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
        adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT};
        adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1, .chan = BATTERY_CHANNEL,
                                               .atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK || adc_oneshot_config_channel(adc, BATTERY_CHANNEL, &chan) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&cal, &cali) != ESP_OK) {
            ESP_LOGW(TAG, "battery: no ADC");
            adc = NULL;
        }
    }
    if (!adc) return 0;
    port_pin_set(PIN_ADC_CTRL, 1);
    port_sleep_ms(5);
    int raw = 0, mv = 0;
    const bool ok = adc_oneshot_read(adc, BATTERY_CHANNEL, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK;
    port_pin_set(PIN_ADC_CTRL, 0);
    /* the divider, 4.9, and Meshtastic's 4.5% for this board (ADC_MULTIPLIER) */
    return ok ? mv * 4900 * 1045 / 1000000 : 0;
}

/* ---- off ---------------------------------------------------------------------------- */

#include "driver/rtc_io.h"
#include "esp_sleep.h"

/* Deep sleep: the OLED and the radio's front end unpowered (Vext high) and the GNSS kit's switch
 * open (run.c's gnss_power), both held so through it; BOOT, pulled up, wakes it into a fresh start.
 * main.c lets the held pins go. */
void port_power_off(void) {
    port_pin_set(PIN_VEXT, 1);
    gpio_hold_en((gpio_num_t)PIN_VEXT);
#ifdef PIN_GNSS_EN
    gpio_hold_en((gpio_num_t)PIN_GNSS_EN);
#endif
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_PAIR_BUTTON, 0);
    rtc_gpio_pullup_en((gpio_num_t)PIN_PAIR_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_PAIR_BUTTON);
    esp_deep_sleep_start();
}
