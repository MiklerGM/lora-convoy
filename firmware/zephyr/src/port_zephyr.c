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

void port_gnss_open(void) {
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
