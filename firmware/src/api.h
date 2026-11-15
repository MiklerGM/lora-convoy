#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "node.h"

/*
 * The phone API (ARCHITECTURE D9): text lines of key=value tokens, the same over Bluetooth, over
 * TCP to a simulated board and on the USB console. Its version is proto= in the status line; an
 * app that does not know it refuses the board (D3).
 *
 *   phone -> board                 board -> phone
 *   STATUS                         STATUS proto=8 board=... kept=<0|1> (also sent unasked)
 *   CLOCK <t1>                     CLOCK t1=<t1> boot=<run> rx=<us> tx=<us> utc=<ms|?>: our clock
 *                                  when the request came and when the answer was made, for the
 *                                  phone to reckon our clock against its own, and our GNSS time
 *                                  then, which it tells the riders' ages by
 *   TIME <ms> <utc ms> <err ms>    OK time=phone err=<ms>: the phone's UTC at our clock's <ms>, a
 *                                  calendar for the riders' ages, never for the slots
 *   RIDERS                         RIDER ... for each rider known, then RIDERS <n>
 *   GROUP <key> <slot> <slots> <ms> OK group=<id> slot= slots= interval=: a group's whole setup,
 *                                  taken whole or not at all, from the invitation
 *   LEDGER                         LEDGER ...: transmit time a minute at a time, over the hour
 *   KEY <32 hex digits>            OK keyed group=<id>: the group's key, from the invitation
 *   STATE <0-15>                   OK state=<n>: what our rider chose (codec.h); HELP <0|1> is states 1 and 0
 *   BEAT <0-7>, MOVE <m>, PASSENGERS <0-3>   the send rule and the records carried, for the bench; not kept
 *   DEAF <3,4|->                   OK deaf=<hex>: slots not taken directly, as if out of reach; the bench's, not kept
 *   SLOT <0-31>, SLOTS <1-32>,     OK <key>=<value>, or ERR <why>
 *   INTERVAL <ms>, POWER <0-3>
 *   MODE <SF 7-12> <kHz>           OK mode=<SF>/<kHz>: the radio mode, for trying range (E2)
 *   FORGET                         FORGOTTEN, over Bluetooth only (ble_nus.c)
 *   REBOOT, BOOTLOADER             -
 *                                  RIDER ... unasked, for every packet taken
 *
 * Commands are case-blind, so the console takes them as typed.
 */
#define API_PROTO 8

/* A buffer for the longest line either way, its NUL in and the newline aside: up to 511
 * characters, as the app takes up to 512 and Zephyr's queue 512 bytes (ble_nus.c). */
#define API_LINE 512

typedef void (*api_reply_fn)(void *ctx, const char *line);

typedef struct {
    node_t *node;
    const char *board;
    const char *reset;              /* why the board last started */
    uint8_t power;                  /* the level the radio is to hold, 0 the strongest */
    int (*tx_dbm)(uint8_t level);   /* what a level puts out on this board */
    /* Set by a command for the caller to carry out, outside the node's lock. */
    bool radio_changed;             /* power or mode: the caller applies the radio again */
    bool setup_changed;             /* key, slots, interval or mode: the caller keeps them over a restart */
    bool save_failed;               /* the caller could not: the status says kept=0 */
    bool forget;
    bool restart;
    bool bootloader;
    uint8_t gnss_ask;               /* GNSS 0|1: 0 nothing asked, 1 off, 2 on */
    uint8_t gnss_systems;           /* SYSTEMS: 1 GPS, 2 BeiDou, 4 GLONASS, summed; 8 two in turn (run.c); 0 as the GNSS has it */
    uint8_t tour_s;                 /* TOUR: the mode changed by the clock every so many seconds; 0 never */
} api_t;

/* One line from the phone or the console. The caller holds the node's lock. came_us: when the line
 * came, before any lock was waited for, which CLOCK answers with; now_us: the command's time, taken
 * under the lock, so no moment the node has kept is after it. */
void api_command(api_t *api, char *line, int64_t came_us, int64_t now_us, api_reply_fn reply, void *ctx);

/* The status line, which the board also sends unasked. */
int api_status(const api_t *api, int64_t now_us, char *buf, size_t cap);

/* TOUR, for trying range (E2): the mode asked for by the clock, SF7/250, SF9/250 and SF10/125 in
 * turn, each for tour_s seconds of UTC, so the boards of a group change together with no word
 * between them. True when the mode asked for changed: the caller applies the radio, as after MODE.
 * Nothing without a time. The caller holds the node's lock. */
bool api_tour(api_t *api, int64_t now_us);
