#pragma once
/*
 * port.h on Zephyr, for the nRF52840 boards: the kernel's clock, mutexes, queues and threads;
 * pins as port * 32 + pin; the radio's bus and chip select from the board's devicetree.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

void port_log(char level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define LOGI(tag, ...) port_log('I', tag, __VA_ARGS__)
#define LOGW(tag, ...) port_log('W', tag, __VA_ARGS__)
#define LOGE(tag, ...) port_log('E', tag, __VA_ARGS__)

static inline int64_t port_uptime_us(void) {
    return (int64_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}
static inline void port_delay_us(uint32_t us) { k_busy_wait(us); }
static inline void port_sleep_ms(uint32_t ms) { k_msleep((int32_t)ms); }

typedef struct k_mutex *port_mutex_t;
port_mutex_t port_mutex_create(void);
static inline bool port_mutex_take(port_mutex_t m, uint32_t timeout_ms) {
    return k_mutex_lock(m, K_MSEC(timeout_ms)) == 0;
}
static inline void port_mutex_lock(port_mutex_t m) { (void)k_mutex_lock(m, K_FOREVER); }
static inline void port_mutex_give(port_mutex_t m) { (void)k_mutex_unlock(m); }

bool port_task_create(void (*fn)(void *), const char *name, size_t stack, int prio);

int port_wdt_watch(void);
void port_wdt_feed(int watch);
void port_wdt_unwatch(int watch);

void port_pin_output(int pin);
void port_pin_input(int pin);
void port_pin_set(int pin, int level);
int port_pin_get(int pin);

void port_radio_spi_init(int nss, int sck, int mosi, int miso);
bool port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len);

bool port_button_down(void);

void port_restart(uint32_t note);
uint32_t port_restart_note(void);
const char *port_reset_cause(void);

void port_console_open(void);
int port_console_getc(uint32_t timeout_ms);

void port_gnss_open(void);
int port_gnss_getc(uint32_t timeout_ms);

uint32_t port_random(void);
size_t port_device_id(uint8_t *out, size_t cap);

/* The UF2 bootloader's drive, on the console's bootloader command: a new image without the reset
 * button's double tap. */
#define PORT_BOOTLOADER 1
void port_restart_to_bootloader(void);
