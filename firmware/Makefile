# The firmware: host tests and the simulator, then the two board images.
#
#   make test          the node core, the SX1262 driver on a fake chip, and the simulator's checks
#   make sim ARGS="10 2000 600"   N riders, every MS, for S seconds: what each heard
#   make esp           the Heltec V4.3 image (PlatformIO, ESP-IDF)
#   make nrf           the Wio Tracker L1 Pro image (Zephyr): zephyr/build/zephyr/zephyr.uf2
#
# The Zephyr workspace is dmd2-lora-bridge's (its firmware/zephyr/setup.sh makes one).

CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2 -Isrc
CORE = src/phy.c src/codec.c src/nmea.c src/node.c
HOST = build/host

test: $(HOST)/test_core $(HOST)/test_sx1262 $(HOST)/sim
	@$(HOST)/test_core
	@$(HOST)/test_sx1262
	@$(HOST)/sim

sim: $(HOST)/sim
	@$(HOST)/sim $(ARGS)

$(HOST)/test_core: test/test_core.c $(CORE) src/*.h | $(HOST)
	@$(CC) $(CFLAGS) -o $@ test/test_core.c $(CORE)

$(HOST)/sim: test/sim.c $(CORE) src/*.h | $(HOST)
	@$(CC) $(CFLAGS) -o $@ test/sim.c $(CORE) -lm

# The driver itself, on ESP-IDF headers left empty: the test defines what it uses. Sign comparison
# is waived because ESP-IDF's own build turns that warning off.
$(HOST)/test_sx1262: test/test_sx1262.c src/sx1262.c src/sx1262.h src/phy.c src/port_esp.h | $(HOST)
	@mkdir -p $(HOST)/idf/driver $(HOST)/idf/freertos
	@cd $(HOST)/idf && touch driver/gpio.h driver/spi_master.h esp_log.h esp_rom_sys.h esp_timer.h \
	    freertos/FreeRTOS.h freertos/semphr.h freertos/task.h
	@$(CC) $(CFLAGS) -Wno-sign-compare -I$(HOST)/idf -o $@ test/test_sx1262.c src/phy.c

$(HOST):
	@mkdir -p $@

esp:
	@pio run -s -e heltec_v4
	@ls -l .pio/build/heltec_v4/firmware.bin

ZEPHYR_WS ?= $(abspath ../../zephyr-ws)
nrf:
	@test -f $(ZEPHYR_WS)/env.sh || { echo "no Zephyr workspace at $(ZEPHYR_WS)"; exit 1; }
	@. $(ZEPHYR_WS)/env.sh && cd $(ZEPHYR_WS) && west build -b wio_tracker_l1 \
	    -d $(CURDIR)/zephyr/build $(CURDIR)/zephyr
	@ls -l zephyr/build/zephyr/zephyr.uf2

.PHONY: test sim esp nrf
