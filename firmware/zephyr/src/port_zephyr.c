/* port.h on Zephyr: the parts that are more than a call through. */
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <hal/nrf_power.h>

#define TAG "port"

/* ---- log ------------------------------------------------------------------ */

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/*
 * Through Zephyr's own log, deferred, so these lines and the host's never interleave and a
 * caller never waits on the USB console - which, with a terminal open and not reading, would
 * otherwise sleep inside a log call. The bench scripts look only at the text after the tag.
 */
void port_log(char level, const char *tag, const char *fmt, ...) {
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    switch (level) {
        case 'E': LOG_ERR("%s: %s", tag, text); break;
        case 'W': LOG_WRN("%s: %s", tag, text); break;
        default: LOG_INF("%s: %s", tag, text); break;
    }
}

/* The string itself to the logger, which copies it whole into its 8 KiB, rather than through
 * port_log's 200 bytes: a STATUS line with E0's txlead= runs to some 480. */
void port_console_line(const char *text) { LOG_INF("line: %s", text); }

/* ---- kernel objects -------------------------------------------------------- */

port_mutex_t port_mutex_create(void) {
    struct k_mutex *m = k_malloc(sizeof(*m));
    if (!m) k_panic();
    k_mutex_init(m);
    return m;
}

struct task_start {
    void (*fn)(void *);
};

static void task_entry(void *start, void *unused1, void *unused2) {
    (void)unused1;
    (void)unused2;
    ((struct task_start *)start)->fn(NULL);
}

/* Priorities are FreeRTOS's, counting up; Zephyr counts down, so 1..6 become 14..9, all
 * preemptible. */
bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio) {
    struct k_thread *thread = k_malloc(sizeof(*thread));
    struct task_start *start = k_malloc(sizeof(*start));
    k_thread_stack_t *stack_mem = k_thread_stack_alloc(stack, 0);
    if (!thread || !start || !stack_mem) return false;
    start->fn = fn;
    k_tid_t tid = k_thread_create(thread, stack_mem, stack, task_entry, start, NULL, NULL,
                                  K_PRIO_PREEMPT(15 - prio), 0, K_NO_WAIT);
    k_thread_name_set(tid, name);
    return true;
}

/* ---- watchdog -------------------------------------------------------------- */

/* What the watchdog and a fatal error leave for the next start when they restart the board, so
 * that start can say task_wdt or panic, as an ESP32 does, rather than a plain sw. */
#define NOTE_TASK_WDT 0x5E70B001u
#define NOTE_PANIC 0x5E70B002u

static void wdt_expired(int channel, void *user_data) {
    (void)channel;
    (void)user_data;
    port_restart(NOTE_TASK_WDT);
}

/*
 * Kernel timers, with the chip's own watchdog under them (prj.conf: 20 s): the task watchdog
 * feeds it from a timer interrupt, so a hang with interrupts off still restarts the board, as
 * reset=wdt. Once started it cannot be stopped, and it runs on through a software restart;
 * the UF2 bootloader feeds it, so an update is not cut short (an L1 Pro sat 45 s in its drive
 * with it running, then took a new image).
 */
int port_wdt_watch(void) {
    static bool ready;
    if (!ready) {
        ready = task_wdt_init(DEVICE_DT_GET(DT_ALIAS(watchdog0))) == 0;
    }
    const int channel = ready ? task_wdt_add(30 * 1000, wdt_expired, NULL) : -1;
    if (channel < 0) LOGW(TAG, "task watchdog unavailable (%d)", channel);
    return channel;
}

void port_wdt_feed(int watch) {
    if (watch >= 0) task_wdt_feed(watch);
}

void port_wdt_unwatch(int watch) {
    if (watch >= 0) task_wdt_delete(watch);
}

/* ---- pins ------------------------------------------------------------------ */

static const struct device *const s_gpio[] = {
    DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

static const struct device *pin_port(int pin) { return s_gpio[pin / 32]; }

void port_pin_output(int pin) { gpio_pin_configure(pin_port(pin), pin % 32, GPIO_OUTPUT); }
void port_pin_input(int pin) { gpio_pin_configure(pin_port(pin), pin % 32, GPIO_INPUT); }
/* Raw: the level on the wire, as the ESP32 builds mean it, not the devicetree's active sense. */
void port_pin_set(int pin, int level) { gpio_pin_set_raw(pin_port(pin), pin % 32, level); }
int port_pin_get(int pin) { return gpio_pin_get_raw(pin_port(pin), pin % 32); }

/* The radio's DIO1, which rises at its TxDone and RxDone (sx1262.c), stamped in its interrupt:
 * E0's measure of when a packet left and came. Its error is the uptime's tick, 31 us at 32768 Hz,
 * plus however long the handler waits to run: to be measured (E0). */
static struct gpio_callback s_dio1_cb;
static struct k_spinlock s_dio1_lock;
static int64_t s_dio1_us;

static void on_dio1(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    (void)dev;
    (void)cb;
    (void)pins;
    const int64_t now = port_uptime_us();
    const k_spinlock_key_t key = k_spin_lock(&s_dio1_lock);
    s_dio1_us = now;
    k_spin_unlock(&s_dio1_lock, key);
}

void port_radio_irq_open(int pin) {
    const struct device *port = pin_port(pin);
    gpio_pin_configure(port, pin % 32, GPIO_INPUT);
    gpio_init_callback(&s_dio1_cb, on_dio1, BIT(pin % 32));
    gpio_add_callback(port, &s_dio1_cb);
    gpio_pin_interrupt_configure(port, pin % 32, GPIO_INT_EDGE_RISING);
}

int64_t port_radio_irq_us(void) {
    const k_spinlock_key_t key = k_spin_lock(&s_dio1_lock);
    const int64_t t = s_dio1_us;
    k_spin_unlock(&s_dio1_lock, key);
    return t;
}

/* ---- the setup, in settings beside the bonds ------------------------------------ */

static uint8_t s_setup[64];
static size_t s_setup_len;
static bool s_setup_found;

static int setup_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "setup") != 0 || len > sizeof(s_setup)) return -ENOENT;
    const ssize_t n = read_cb(cb_arg, s_setup, len);
    if (n < 0) return (int)n;
    s_setup_len = (size_t)n;
    s_setup_found = true;
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(convoy, "convoy", NULL, setup_set, NULL, NULL);

/* Loaded only after main has read the whole store (store_scan_bonds): Zephyr's loader deletes
 * whatever it cannot read, whichever subtree it was asked for. */
bool port_setup_load(void *buf, size_t len) {
    s_setup_found = false;
    if (settings_load_subtree("convoy") != 0 || !s_setup_found || s_setup_len != len) return false;
    memcpy(buf, s_setup, len);
    return true;
}

bool port_setup_save(const void *buf, size_t len) { return settings_save_one("convoy/setup", buf, len) == 0; }

/* ---- the button --------------------------------------------------------------- */

static const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* Logical, so the devicetree's polarity and pull apply; configured on first use. */
bool port_button_down(void) {
    static bool ready;
    if (!ready) {
        ready = gpio_pin_configure_dt(&s_button, GPIO_INPUT) == 0;
    }
    return ready && gpio_pin_get_dt(&s_button) == 1;
}

/* System OFF, the button set to wake it into a fresh start. The T-Echo's peripherals lose their
 * power: its e-paper, done drawing, keeps the last picture, and its lines are let go first so nothing
 * leaks into it (Meshtastic's variant_shutdown). Elsewhere run.c has put them to sleep. The
 * bootloader would take the button held as it wakes for a call to it: Adafruit's DFU_MAGIC_SKIP
 * tells it not to look, as Meshtastic's cpuDeepSleep does. */
void port_power_off(void) {
#ifdef PIN_POWER_EN
    for (int i = 0; i < 300 && port_pin_get(PIN_EPD_BUSY); i++) k_msleep(10);
    port_pin_input(PIN_EPD_CS);
    port_pin_input(PIN_EPD_DC);
    port_pin_input(PIN_EPD_RES);
    port_pin_input(PIN_EPD_BUSY);
    port_pin_set(PIN_POWER_EN, 0);
    port_pin_input(PIN_POWER_EN);
#endif
    nrf_power_gpregret_set(NRF_POWER, 0, 0x6d);
    gpio_pin_configure_dt(&s_button, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&s_button, GPIO_INT_LEVEL_ACTIVE);
    sys_poweroff();
}

/* ---- the radio's bus -------------------------------------------------------- */

static struct spi_dt_spec s_spi =
    SPI_DT_SPEC_GET(DT_ALIAS(lora0), SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_CONTROLLER);

/* The devicetree names the bus and the chip select; the clock is held at the ESP32 builds' 2 MHz,
 * so the two drivers time the chip alike. The pin arguments are theirs. */
void port_radio_spi_init(int nss, int sck, int mosi, int miso) {
    (void)nss;
    (void)sck;
    (void)mosi;
    (void)miso;
    if (!spi_is_ready_dt(&s_spi)) k_panic();
    s_spi.config.frequency = 2 * 1000 * 1000;
}

bool port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    const struct spi_buf tx_buf = {.buf = (void *)tx, .len = len};
    const struct spi_buf rx_buf = {.buf = rx, .len = len};
    const struct spi_buf_set txs = {.buffers = &tx_buf, .count = 1};
    const struct spi_buf_set rxs = {.buffers = &rx_buf, .count = 1};
    return spi_transceive_dt(&s_spi, &txs, &rxs) == 0;
}

/* ---- reset and restart -------------------------------------------------------- */

/*
 * Survives a software reset - RAM is not cleared - but not a power loss. Whether the UF2
 * bootloader, which runs on every reset, leaves this word alone is to be confirmed on the board;
 * if it does not, the cost is reset=sw where ble_stuck or bond_reset was meant.
 */
static __noinit uint32_t s_restart_note;

void port_restart(uint32_t note) {
    s_restart_note = note;
    sys_reboot(SYS_REBOOT_COLD);
}

/* Adafruit's UF2 bootloader, the one the L1 ships with, stays in its drive mode when it starts
 * with this in GPREGRET. Zephyr's sys_reboot() ignores its type on the nRF52, so it is set here. */
#define UF2_BOOTLOADER_MAGIC 0x57

void port_restart_to_bootloader(void) {
    nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
    sys_reboot(SYS_REBOOT_COLD);
}

/*
 * In place of Zephyr's, which halts: a halted board is off the air until someone re-plugs it,
 * and the kernel's watchdog, the only one running, halts with it. A restart is back in seconds.
 */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    (void)reason;
    (void)esf;
    port_restart(NOTE_PANIC);
}

/* Read once, then cleared, so a note never outlives the restart it was left for. */
static uint32_t note_once(void) {
    static bool read;
    static uint32_t note;
    if (!read) {
        read = true;
        note = s_restart_note;
        s_restart_note = 0;
    }
    return note;
}

uint32_t port_restart_note(void) { return note_once(); }

/* The nRF52's reset reasons, in ESP-IDF's words where there is one. A brownout looks like a
 * power-on here: the chip keeps no separate flag for it. */
const char *port_reset_cause(void) {
    static const char *cause;
    if (cause) return cause;
    uint32_t flags = 0;
    if (hwinfo_get_reset_cause(&flags) != 0) flags = 0;
    (void)hwinfo_clear_reset_cause();   /* they accumulate across resets until cleared */
    if (flags & RESET_WATCHDOG) {
        cause = "wdt";
    } else if (flags & RESET_CPU_LOCKUP) {
        cause = "panic";
    } else if (flags & RESET_SOFTWARE) {
        const uint32_t note = note_once();
        cause = note == NOTE_TASK_WDT ? "task_wdt" : note == NOTE_PANIC ? "panic" : "sw";
    } else if (flags & RESET_PIN) {
        cause = "pin";
    } else if (flags == 0) {
        cause = "poweron";
    } else {
        cause = "other";
    }
    return cause;
}

/* ---- the GNSS -------------------------------------------------------------- */

/* uart0, which app.overlay takes from Zephyr's GNSS driver so that both boards read NMEA alike. At
 * 9600 baud a byte comes every millisecond, faster than polling with sleeps would catch, so the
 * interrupt fills a ring and the GNSS task drains it. */
static const struct device *const s_gnss = DEVICE_DT_GET(DT_NODELABEL(uart0));
RING_BUF_DECLARE(s_gnss_ring, 512);
static K_SEM_DEFINE(s_gnss_data, 0, 1);

static void gnss_isr(const struct device *dev, void *user_data) {
    (void)user_data;
    uart_irq_update(dev);
    while (uart_irq_rx_ready(dev)) {
        uint8_t buf[32];
        const int n = uart_fifo_read(dev, buf, sizeof(buf));
        if (n <= 0) break;
        ring_buf_put(&s_gnss_ring, buf, (uint32_t)n);   /* a full ring drops: a sentence is lost */
        k_sem_give(&s_gnss_data);
    }
}

/* The PPS edge, stamped in its interrupt as DIO1 is, on a board that wires it (board.h). */
static struct k_spinlock s_pps_lock;
static int64_t s_pps_us;

#ifdef PIN_GNSS_PPS
static struct gpio_callback s_pps_cb;

static void on_pps(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    (void)dev;
    (void)cb;
    (void)pins;
    const int64_t now = port_uptime_us();
    const k_spinlock_key_t key = k_spin_lock(&s_pps_lock);
    s_pps_us = now;
    k_spin_unlock(&s_pps_lock, key);
}
#endif

int64_t port_pps_us(void) {
    const k_spinlock_key_t key = k_spin_lock(&s_pps_lock);
    const int64_t t = s_pps_us;
    k_spin_unlock(&s_pps_lock, key);
    return t;
}

void port_gnss_open(void) {
#ifdef PIN_GNSS_PPS
    const struct device *port = pin_port(PIN_GNSS_PPS);
    gpio_pin_configure(port, PIN_GNSS_PPS % 32, GPIO_INPUT);
    gpio_init_callback(&s_pps_cb, on_pps, BIT(PIN_GNSS_PPS % 32));
    gpio_add_callback(port, &s_pps_cb);
    gpio_pin_interrupt_configure(port, PIN_GNSS_PPS % 32, GPIO_INT_EDGE_RISING);
#endif
    const struct uart_config cfg = {.baudrate = 9600, .parity = UART_CFG_PARITY_NONE,
                                    .stop_bits = UART_CFG_STOP_BITS_1, .data_bits = UART_CFG_DATA_BITS_8,
                                    .flow_ctrl = UART_CFG_FLOW_CTRL_NONE};
    if (!device_is_ready(s_gnss) || uart_configure(s_gnss, &cfg) != 0) {
        LOGE(TAG, "the GNSS's UART is not ready");
        return;
    }
    uart_irq_callback_user_data_set(s_gnss, gnss_isr, NULL);
    uart_irq_rx_enable(s_gnss);
}

void port_gnss_write(const char *s) {
    while (*s) uart_poll_out(s_gnss, (unsigned char)*s++);
}

static int gnss_take(void) {
    uint8_t c;
    const unsigned key = irq_lock();
    const uint32_t n = ring_buf_get(&s_gnss_ring, &c, 1);
    irq_unlock(key);
    return n == 1 ? c : -1;
}

int port_gnss_getc(uint32_t timeout_ms) {
    int c = gnss_take();
    if (c < 0) {
        k_sem_take(&s_gnss_data, K_MSEC(timeout_ms));
        c = gnss_take();
    }
    return c;
}

uint32_t port_random(void) { return sys_rand32_get(); }

size_t port_device_id(uint8_t *out, size_t cap) {
    const ssize_t n = hwinfo_get_device_id(out, cap);
    return n > 0 ? (size_t)n : 0;
}

/* ---- the console ------------------------------------------------------------ */

static const struct device *const s_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

void port_console_open(void) {}

/* The console is the board's own USB, as on an ESP32-S3; polled, since only a person reads it. */
int port_console_getc(uint32_t timeout_ms) {
    const int64_t until = k_uptime_get() + timeout_ms;
    do {
        unsigned char c;
        if (uart_poll_in(s_console, &c) == 0) return c;
        k_msleep(10);
    } while (k_uptime_get() < until);
    return -1;
}

/* ---- the screen and the battery, as the board's devicetree names them -------------------------- */

#if DT_HAS_CHOSEN(zephyr_display)
#include <zephyr/drivers/display.h>

static const struct device *const s_display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
#define EPD_WHOLE_EVERY 20

/* screen.c's pages, a byte for eight rows with the top row in the low bit; a driver that tiles
 * otherwise is not one this takes. */
bool port_screen_size(int *w, int *h) {
    if (!device_is_ready(s_display)) return false;
    struct display_capabilities caps;
    display_get_capabilities(s_display, &caps);
    *w = caps.x_resolution;
    *h = caps.y_resolution;
    return (caps.screen_info & SCREEN_INFO_MONO_VTILED) && *w * *h / 8 <= 200 * 200 / 8;
}

/* Turned round as the driver wants it: the top row in the high bit where it says MSB first, and
 * the letters dark on an e-paper, lit on an OLED, whichever of white or black its format calls 1. */
void port_screen_show(const uint8_t *buf) {
    static uint8_t out[200 * 200 / 8];
    static bool on;
    struct display_capabilities caps;
    display_get_capabilities(s_display, &caps);
    const size_t len = (size_t)caps.x_resolution * caps.y_resolution / 8;
    const bool msb = caps.screen_info & SCREEN_INFO_MONO_MSB_FIRST;
    const bool invert = !(caps.screen_info & SCREEN_INFO_EPD) != !(caps.current_pixel_format == PIXEL_FORMAT_MONO10);
    for (size_t i = 0; i < len && i < sizeof(out); i++) {
        uint8_t b = buf[i];
        if (msb) {
            b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
            b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
            b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
        }
        out[i] = invert ? (uint8_t)~b : b;
    }
    const struct display_buffer_descriptor desc = {
        .buf_size = len, .width = caps.x_resolution, .height = caps.y_resolution, .pitch = caps.x_resolution};
    /* An e-paper draws its first picture whole, on blanking off, and the later ones in part; each
     * EPD_WHOLE_EVERY-th whole again, written while blanked, which clears the ghosts the partial ones
     * leave (Meshtastic's T-Echo: 20 in part at most). */
    static unsigned drawn;
    const bool whole = on && (caps.screen_info & SCREEN_INFO_EPD) && ++drawn % EPD_WHOLE_EVERY == 0;
    if (whole && display_blanking_on(s_display) != 0) LOGW(TAG, "screen: no whole redraw");
    if (display_write(s_display, 0, 0, &desc, out) != 0) LOGW(TAG, "screen: write refused");
    if (!on || whole) on = display_blanking_off(s_display) == 0;
}

/* Dark by the driver's blanking: an OLED's display off, its memory kept. An e-paper keeps its picture
 * unpowered, so it is never dark, and goes on being redrawn. */
bool port_screen_light(bool on) {
    if (!device_is_ready(s_display)) return false;
    struct display_capabilities caps;
    display_get_capabilities(s_display, &caps);
    if (caps.screen_info & SCREEN_INFO_EPD) return false;
    return (on ? display_blanking_off(s_display) : display_blanking_on(s_display)) == 0;
}
#else
bool port_screen_size(int *w, int *h) {
    (void)w, (void)h;
    return false;
}
void port_screen_show(const uint8_t *buf) { (void)buf; }
bool port_screen_light(bool on) {
    (void)on;
    return false;
}
#endif

/* The nRF52840's own VBUS detector, which a charging cell's voltage is not: Meshtastic's T-Echo
 * reads the voltage, and shows a charging cell as a percentage. */
#include <hal/nrf_power.h>
bool port_usb_power(void) { return nrf_power_usbregstatus_vbusdet_get(NRF_POWER); }

#if DT_HAS_COMPAT_STATUS_OKAY(voltage_divider)
#include <zephyr/drivers/sensor.h>

static const struct device *const s_vbatt = DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(voltage_divider));

int port_battery_mv(void) {
    struct sensor_value v;
    if (!device_is_ready(s_vbatt) || sensor_sample_fetch(s_vbatt) != 0 ||
        sensor_channel_get(s_vbatt, SENSOR_CHAN_VOLTAGE, &v) != 0) {
        return 0;
    }
    return v.val1 * 1000 + v.val2 / 1000;
}
#else
int port_battery_mv(void) { return 0; }
#endif
