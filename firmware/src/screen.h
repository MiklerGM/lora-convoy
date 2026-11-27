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
 *                    call for help (R3 SOS) or to stop, which matters more; else "charge me" for
 *                    a cell nearly empty
 *   R4 now via R6    each rider known, how long since, its state, who told: the e-paper's only
 *
 * The satellites and the riders' ages move by the second, so they wait for the clock's minute
 * (screen_due): an e-paper redraws about once a minute, and at once for a state, a call for help or
 * a press of the button.
 */
#define SCREEN_LINES 12
#define SCREEN_COLS 20
#define SCREEN_FONT_W 10
#define SCREEN_FONT_H 16
#define SCREEN_BYTES (200 * 200 / 8) /* the largest screen's buffer */

typedef char screen_text_t[SCREEN_LINES][SCREEN_COLS + 1];

/* The caller holds the node's lock. phone: one is connected; battery_mv under 2500 for none, over
 * SCREEN_USB_MV for a divider that sees USB, not the cell; usb: the chip says USB power is in;
 * zone_min the local time's offset from UTC. */
#define SCREEN_USB_MV 4400
void screen_text(const node_t *n, int64_t now_us, bool phone, int battery_mv, bool usb, int zone_min, screen_text_t out);

/* Whether a screen showing shown is redrawn for text: for the clock's minute, the phone or the battery
 * (line 0), our state (1), a call for help, a stop or the riders heard (3), the rest coming along;
 * not for the satellites in the fix (2) or a rider's age (4 on) alone, which move by the second:
 * the T-Echo's fix or its count changed 95 times in 70 min (2026-11-23), each an e-paper's redraw. */
bool screen_due(const screen_text_t shown, const screen_text_t text);

/* In a line of text, a charging cell's mark, drawn as a bolt: not a letter of the font. */
#define SCREEN_CHARGING '\x7f'
extern const uint8_t screen_bolt[20], screen_bolt_large[45];

/* A lithium cell's charge from its voltage, roughly: it reads high while charging or under no load. */
int screen_battery_pct(int mv);

/* A cell read below mv, on a board with one and off USB [owner, 2026-11-26]. Below BATTERY_LOW_MV the
 * screen asks for a charge where the riders heard were; below BATTERY_EMPTY_MV the board goes off
 * (run.c) before the cell browns it out: the T-Echo, out half charged on 2026-11-25, browned out and
 * started again 38 times in 4 h, its riders seeing it come and go. The L1, every 2 s, had some 2 h
 * left at 3.35 V and 1 h at 3.23 V (07-experiments). ponytail: the T-Echo's own bottom is unmeasured;
 * 3.2 V leaves its smaller cell a margin, tune it when a run-down of it is logged. */
#define BATTERY_LOW_MV 3350
#define BATTERY_EMPTY_MV 3200
bool screen_battery_below(int battery_mv, bool usb, int mv);

/* Into buf, w * h / 8 bytes, h a multiple of 8: a byte for eight rows of one column, the top row in
 * its low bit, a band of eight rows across the screen after another (an SSD1306's pages). A 1 is the
 * letters: lit on an OLED, inked on an e-paper, whose platform turns it round. */
void screen_draw(const screen_text_t text, uint8_t *buf, int w, int h);
