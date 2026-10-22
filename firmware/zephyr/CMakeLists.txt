# LoRa Convoy on Zephyr, for the nRF52840 boards. The shared sources are the ESP32 build's own, from
# ../src; what differs is the platform under them (src/port_zephyr.*) and start-up (src/main.c).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_convoy)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_sources(app PRIVATE
    src/main.c
    src/port_zephyr.c
    ${SHARED}/run.c
    ${SHARED}/sx1262.c
    ${SHARED}/phy.c
    ${SHARED}/codec.c
    ${SHARED}/nmea.c
    ${SHARED}/node.c
)
target_include_directories(app PRIVATE src ${SHARED})
# The board's pins, from its devicetree, ahead of everything else: sx1262.h's defaults are the
# Heltec's and apply to whatever has not been named first.
target_compile_options(app PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/board.h)
