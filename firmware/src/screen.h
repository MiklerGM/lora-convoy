#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "node.h"

/*
 * What a board shows on its screen (BACKLOG, T-board-ui): a few lines of text from the node's
 * state, drawn in Zephyr's letters (screen_font.c) into a buffer the platform hands its screen
 * (port_screen_show). The OLEDs of the Heltec and the L1, 128 x 64, show the first 4 lines to 12
 * letters of 10 x 16; the T-Echo's e-paper, 200 x 200, 8 lines of 13 in 15 x 24. So the lines come
 * in order of what a rider needs first, and none needs more than 13 letters to say it:
 *
 *   13:05 BT 87%     the time where the phone is (ZONE), a phone connected, the battery
 *   R5/10 SOS        who we are in the group, and the state we say if not riding
 *   on air 8sat      whether the board sends (its time fits the slot) or is silent, and its fix
 *   riders 2/9       others heard in the last minute, of those in the group; or another rider's
 *                    call for help (R3 SOS) or to stop, which matters more
 *   R4 now via R6    each rider known, how long since, its state, who told: the e-paper's only
 *
 * Nothing in them changes by the second, so an e-paper redraws at most once a minute, for the
 * clock, unless something else has changed.
 */
#define SCREEN_LINES 12
#define SCREEN_COLS 20
#define SCREEN_FONT_W 10
#define SCREEN_FONT_H 16
#define SCREEN_BYTES (200 * 200 / 8) /* the largest screen's buffer */

typedef char screen_text_t[SCREEN_LINES][SCREEN_COLS + 1];

/* The caller holds the node's lock. phone: one is connected; battery_mv under 2500 for none; zone_min
 * the local time's offset from UTC. */
void screen_text(const node_t *n, int64_t now_us, bool phone, int battery_mv, int zone_min, screen_text_t out);

/* A lithium cell's charge from its voltage, roughly: it reads high while charging or under no load. */
int screen_battery_pct(int mv);

/* Into buf, w * h / 8 bytes, h a multiple of 8: a byte for eight rows of one column, the top row in
 * its low bit, a band of eight rows across the screen after another (an SSD1306's pages). A 1 is the
 * letters: lit on an OLED, inked on an e-paper, whose platform turns it round. */
void screen_draw(const screen_text_t text, uint8_t *buf, int w, int h);
