#pragma once

/*
 * The firmware every board runs, once its own start-up has powered what it carries and opened its
 * storage: the GNSS read into the node core, the radio sending what the core asks and handing it
 * what arrives, Bluetooth and the console for the phone API, and the button. reset is
 * run_reset_name()'s answer. Returns once the tasks are running.
 */
void run_start(const char *board, const char *reset);

/* What a restart the firmware chooses leaves for the next start (port_restart), so that start can
 * say why it happened rather than pass for a fresh power-up. */
#define RESTART_NOTE_MAGIC 0x5E70C000u
enum { RESTART_FOR_BLE = 1, RESTART_FOR_BOND = 2 };

/* Hold the board's button this long to forget the paired phone. */
#define PAIR_HOLD_MS 3000

/* Why the board last started, as status lines report it in reset=: ble_stuck and bond_reset as
 * well as the chip's own causes. Call once, at startup: a restart note is consumed by reading it. */
const char *run_reset_name(void);

/*
 * Storage that will not open is not erased on the firmware's own say-so: the bonds live there, and
 * an empty bond store means nobody owns the board, so the next phone to pair would claim it.
 * Bluetooth waits for someone at the board to hold the button - the gesture that forgets the phone
 * anyway. Returns once erase() has run and the button is released. From dmd2-lora-bridge.
 */
void run_erase_when_asked(const char *why, int watch, void (*erase)(void));
