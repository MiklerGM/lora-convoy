#pragma once

/*
 * The firmware every board runs, once its own start-up has powered what it carries: the GNSS read
 * into the node core, the radio sending what the core asks and handing it what arrives, and the
 * console. Returns once the tasks are running.
 */
void run_start(const char *board);
