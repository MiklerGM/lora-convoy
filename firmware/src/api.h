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
 *   STATUS                         STATUS proto=3 board=... (also sent unasked)
 *   RIDERS                         RIDER ... for each rider known, then RIDERS <n>
 *   KEY <32 hex digits>            OK keyed group=<tag>: the group's key, from the invitation
 *   SLOT <0-31>, SLOTS <1-32>,     OK <key>=<value>, or ERR <why>
 *   INTERVAL <ms>, POWER <0-3>
 *   FORGET                         FORGOTTEN, over Bluetooth only (ble_nus.c)
 *   REBOOT, BOOTLOADER             -
 *                                  RIDER ... unasked, for every packet taken
 *
 * Commands are case-blind, so the console takes them as typed.
 */
#define API_PROTO 3

/* The longest line either way, newline aside; the app takes up to 512 characters. */
#define API_LINE 480

typedef void (*api_reply_fn)(void *ctx, const char *line);

typedef struct {
    node_t *node;
    const char *board;
    const char *reset;              /* why the board last started */
    uint8_t power;                  /* the level the radio is to hold, 0 the strongest */
    int (*tx_dbm)(uint8_t level);   /* what a level puts out on this board */
    /* Set by a command for the caller to carry out, outside the node's lock. */
    bool power_changed;
    bool setup_changed;             /* key, slots or interval: the caller keeps them over a restart */
    bool forget;
    bool restart;
    bool bootloader;
} api_t;

/* One line from the phone or the console. The caller holds the node's lock. */
void api_command(api_t *api, char *line, int64_t now_us, api_reply_fn reply, void *ctx);

/* The status line, which the board also sends unasked. */
int api_status(const api_t *api, int64_t now_us, char *buf, size_t cap);
