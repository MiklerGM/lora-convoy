#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the shared firmware asks of the platform under it: ESP-IDF on the Heltec V4.3, Zephyr on
 * the Wio Tracker L1 Pro. From dmd2-lora-bridge at 2234b5a, plus the GNSS's UART, a random number
 * and the chip's own ID.
 *
 * Each platform supplies the same names, selected by the build. Pins are plain numbers: a GPIO
 * number on an ESP32, port * 32 + pin on an nRF52.
 *
 *   int64_t port_uptime_us(void)                monotonic, since boot
 *   void    port_delay_us(uint32_t us)          busy wait, for the radio's sub-millisecond timing
 *   void    port_sleep_ms(uint32_t ms)          blocks the caller; others run meanwhile
 *
 *   port_mutex_t port_mutex_create(void)        does not return without one
 *   bool    port_mutex_take(port_mutex_t, uint32_t timeout_ms)
 *   void    port_mutex_lock(port_mutex_t)       waits for as long as it takes
 *   void    port_mutex_give(port_mutex_t)
 *
 *   bool    port_task_create(void (*fn)(void *), const char *name, size_t stack_bytes, int prio)
 *                                               prio as FreeRTOS counts it: higher runs first
 *
 *   int     port_wdt_watch(void)                restart the board if the calling task stops
 *   void    port_wdt_feed(int watch)            feeding this for 30 s
 *
 *   void    port_pin_output(int pin)            no pulls
 *   void    port_pin_input(int pin)             no pulls
 *   void    port_pin_set(int pin, int level)    the physical level, whatever the board calls active
 *   int     port_pin_get(int pin)
 *
 *   void    port_radio_spi_init(int nss, int sck, int mosi, int miso)
 *   bool    port_radio_spi_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
 *   void    port_radio_irq_open(int pin)        the radio's DIO1, its rising edge stamped in an
 *   int64_t port_radio_irq_us(void)             interrupt: port_uptime_us at the last one, 0 before
 *
 *   void    port_gnss_open(void)                the GNSS's UART, 9600 8N1, on the board's pins
 *   int     port_gnss_getc(uint32_t timeout_ms) the next byte from the GNSS, or -1
 *   void    port_gnss_write(const char *s)      a line to the GNSS, a configuration command
 *   int64_t port_pps_us(void)                   port_uptime_us at the GNSS's last PPS edge; 0 before
 *                                               one, and always on a board without the line
 *
 *   uint32_t port_random(void)                  from the chip's own generator
 *   size_t  port_device_id(uint8_t *out, size_t cap)   bytes that tell this chip from another
 *
 *   bool    port_setup_load(void *buf, size_t len)   the setup run.c keeps over a restart:
 *   bool    port_setup_save(const void *buf, size_t len) the group's key, the slot, the interval
 *
 *   bool    port_button_down(void)              the board's one button, held
 *   void    port_power_off(void)                the board off, what it carries unpowered or asleep;
 *                                               its button starts it again, as at power-up. Never returns
 *   bool    port_touch_down(void)               the T-Echo's touch pad, held; false elsewhere
 *   void    port_backlight(bool on)             the T-Echo's screen light; nothing elsewhere
 *
 *   bool    port_screen_size(int *w, int *h)    the screen's pixels; false on a board without one
 *   void    port_screen_show(const uint8_t *buf) a picture as screen_draw makes it for that size
 *   bool    port_screen_light(bool on)          lit or dark, keeping its picture; false where it
 *                                               cannot be dark (none, or an e-paper, which needs no power)
 *   int     port_battery_mv(void)               the battery's voltage; 0 where the board cannot tell
 *   bool    port_usb_power(void)                USB power in; false where the chip cannot tell
 *
 *   void    port_restart(uint32_t note)         restarts, leaving the note for the next start
 *   uint32_t port_restart_note(void)            that note once, then 0; 0 after a power loss
 *   const char *port_reset_cause(void)          poweron brownout panic int_wdt task_wdt wdt pin
 *                                               sw other - in ESP-IDF's words where it has one
 *
 *   void    port_console_open(void)             the host's side of the console
 *   void    port_console_line(const char *text) a line of the phone API's to it, whole, up to
 *                                               API_LINE, as LOGI cuts its own lines shorter
 *   int     port_console_getc(uint32_t timeout_ms)   the next byte, or -1
 *
 *   LOGI(tag, fmt, ...), LOGW(...), LOGE(...)   one line each
 */
#ifdef __ZEPHYR__
#include "port_zephyr.h"
#else
#include "port_esp.h"
#endif
