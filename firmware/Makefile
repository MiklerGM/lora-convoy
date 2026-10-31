# The firmware: host tests and the simulator, then the two board images.
#
#   make test          the node core, at 64 bits and at the boards' 32 (gcc-multilib), the SX1262
#                      driver on a fake chip, and the simulator's checks
#   make sim ARGS="10 2000 600"   N riders, every MS, for S seconds: what each heard
#   make esp           the Heltec V4.3 image (PlatformIO, ESP-IDF)
#   make nrf           the Wio Tracker L1 Pro image (Zephyr): zephyr/build/zephyr/zephyr.uf2
#
# The Zephyr workspace is dmd2-lora-bridge's (its firmware/zephyr/setup.sh makes one).

CC ?= gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O2 -Isrc
CORE = src/phy.c src/crypto.c src/codec.c src/nmea.c src/node.c src/api.c
HOST = build/host

test: $(HOST)/test_core $(HOST)/test_core32 $(HOST)/test_sx1262 $(HOST)/sim
	@$(HOST)/test_core
	@$(HOST)/test_core32 >/dev/null
	@$(HOST)/test_sx1262
	@$(HOST)/sim
	@python3 ../tools/test_compare.py
	@$(MAKE) -s test-simulator

sim: $(HOST)/sim
	@$(HOST)/sim $(ARGS)

compare: $(HOST)/sim
	@python3 ../tools/compare.py $(ARGS)

simulator: $(HOST)/libconvoy_sim.so
	@python3 ../tools/simulate.py $(ARGS)

test-simulator: $(HOST)/libconvoy_sim.so
	@cd .. && python3 -m unittest discover -s tools/simulator -t .

$(HOST)/libconvoy_sim.so: sim/bridge.c $(CORE) src/*.h | $(HOST)
	@$(CC) $(CFLAGS) -fPIC -shared -o $@ sim/bridge.c $(CORE)

$(HOST)/test_core: test/test_core.c $(CORE) src/*.h | $(HOST)
	@$(CC) $(CFLAGS) -o $@ test/test_core.c $(CORE)

# Again at 32 bits, as the boards have it: there a long holds no more than 25 days of ms.
$(HOST)/test_core32: test/test_core.c $(CORE) src/*.h | $(HOST)
	@$(CC) -m32 $(CFLAGS) -o $@ test/test_core.c $(CORE)

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

.PHONY: test sim compare simulator test-simulator esp nrf
