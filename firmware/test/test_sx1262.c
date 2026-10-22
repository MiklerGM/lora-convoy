/*
 * The SX1262 driver against a fake chip at the far end of its SPI bus, from dmd2-lora-bridge: what
 * radio_verify calls healthy has to be what the chip was last told, and the mode the protocol names
 * has to be what reaches the chip's registers.
 *
 * sx1262.c is compiled whole, on just enough of ESP-IDF to build (the Makefile supplies its headers
 * as empty files). The fake keeps only what the radio would: registers, frequency, modulation,
 * mode, and how its last command went - told, as the real chip tells it, on the transfer after.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- just enough ESP-IDF ---------------------------------------------------- */

typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef void *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num,
                 max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
typedef void *SemaphoreHandle_t;

#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define SPI2_HOST 1
#define SPI_DMA_CH_AUTO 3
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)
#define configASSERT(x) assert(x)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

static void log_nothing(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(tag, ...) log_nothing(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_nothing(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) log_nothing(tag, __VA_ARGS__)

static int gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static int gpio_set_level(gpio_num_t pin, int level) { (void)pin; (void)level; return ESP_OK; }
static int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }   /* BUSY is never high */
static void esp_rom_delay_us(uint32_t us) { (void)us; }
/* A millisecond per look at the clock, so every deadline in the driver arrives. */
static int64_t esp_timer_get_time(void) { static int64_t now; return now += 1000; }
static void vTaskDelay(int ticks) { (void)ticks; }
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static int xSemaphoreTake(SemaphoreHandle_t s, int ticks) { (void)s; (void)ticks; return pdTRUE; }
static int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static int spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma) {
    (void)host; (void)bus; (void)dma;
    return ESP_OK;
}
static int spi_bus_add_device(int host, const spi_device_interface_config_t *dev,
                              spi_device_handle_t *out) {
    (void)host; (void)dev;
    *out = (spi_device_handle_t)1;
    return ESP_OK;
}
static int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t);

#include "../src/sx1262.c"

/* ---- the chip ---------------------------------------------------------------- */

#define MODE_STANDBY 2
#define MODE_RX 5

static struct {
    uint8_t regs[0x10000];
    uint8_t frequency[4];   /* the last SetRfFrequency it took */
    uint8_t modulation[4];  /* the last SetModulationParams: SF, BW, CR, LDRO */
    uint8_t txmod_at_tx;    /* errata 15.1's register when the last SetTx came */
    uint8_t mode;
    uint8_t status;         /* how the last command went, told on the next transfer */
    uint8_t drop;           /* a command the bus fails, once */
    uint8_t refuse;         /* a command the chip refuses, once */
} chip = {.mode = MODE_STANDBY};

static int spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t) {
    (void)spi;
    const uint8_t *tx = t->tx_buffer;
    uint8_t *rx = t->rx_buffer;
    const size_t n = t->length / 8;
    if (chip.drop && tx[0] == chip.drop) {
        chip.drop = 0;
        return -1;
    }
    if (rx) {
        memset(rx, 0, n);
        if (n > 1) rx[1] = (uint8_t)(chip.mode << 4 | chip.status << 1);
    }
    chip.status = 0;
    if (chip.refuse && tx[0] == chip.refuse) {
        chip.refuse = 0;
        chip.status = 5;    /* failure to execute: nothing changes */
        return ESP_OK;
    }
    switch (tx[0]) {
        case CMD_WRITE_REGISTER: memcpy(&chip.regs[tx[1] << 8 | tx[2]], &tx[3], n - 3); break;
        case CMD_READ_REGISTER: memcpy(&rx[4], &chip.regs[tx[1] << 8 | tx[2]], n - 4); break;
        case CMD_SET_RF_FREQUENCY: memcpy(chip.frequency, &tx[1], 4); break;
        case CMD_SET_MODULATION: memcpy(chip.modulation, &tx[1], 4); break;
        case CMD_SET_TX:
            chip.txmod_at_tx = chip.regs[REG_TX_MODULATION];
            chip.mode = MODE_STANDBY;   /* a packet goes out at once */
            break;
        case CMD_SET_STANDBY: chip.mode = MODE_STANDBY; break;
        case CMD_SET_RX: chip.mode = MODE_RX; break;
        case CMD_GET_IRQ_STATUS: rx[3] = IRQ_TX_DONE; break;
    }
    return ESP_OK;
}

int main(void) {
    phy_t other = PHY_V0;
    other.freq_hz = 868100000;
    other.sf = 11;
    other.bw_hz = 125000;
    radio_init();
    assert(radio_apply(&PHY_V0, 3) && radio_verify());

    /* What v0 names is what the chip holds: 869.525 MHz, SF7, 250 kHz, 4/5, no LDRO, our sync
     * word; and errata 15.1's bit set at 250 kHz when a packet leaves. */
    static const uint8_t f869525[4] = {0x36, 0x58, 0x66, 0x66};
    static const uint8_t sf7_250[4] = {7, 0x05, 0x01, 0x00};
    assert(memcmp(chip.frequency, f869525, 4) == 0 && memcmp(chip.modulation, sf7_250, 4) == 0);
    assert(chip.regs[REG_LORA_SYNC_MSB] == 0x64 && chip.regs[REG_LORA_SYNC_MSB + 1] == 0x74);
    assert(radio_send((const uint8_t *)"0123456789abcdefg", 17) && (chip.txmod_at_tx & 0x04));
    printf("sx1262: v0's mode reaches the chip's registers\n");

    /* Another mode moves everything, LDRO included at 16 ms symbols. */
    static const uint8_t f868100[4] = {0x36, 0x41, 0x99, 0x99};
    static const uint8_t sf11_125[4] = {11, 0x04, 0x01, 0x01};
    assert(radio_apply(&other, 0) && radio_verify());
    assert(memcmp(chip.frequency, f868100, 4) == 0 && memcmp(chip.modulation, sf11_125, 4) == 0);
    assert(radio_apply(&PHY_V0, 3));

    /* The frequency lands and a later command does not, lost on the bus or refused by the chip -
     * which says so only on the transfer after it. Neither mode may be confirmed until a whole
     * apply succeeds. */
    static const struct { uint8_t *how, opcode; } partway[] = {
        {&chip.drop, CMD_SET_MODULATION}, {&chip.refuse, CMD_SET_TX_PARAMS}};
    for (size_t i = 0; i < sizeof(partway) / sizeof(partway[0]); i++) {
        *partway[i].how = partway[i].opcode;
        assert(!radio_apply(&other, 0));
        assert(memcmp(chip.frequency, f868100, 4) == 0);   /* the chip has left v0's frequency */
        assert(!radio_verify());
        assert(radio_apply(&PHY_V0, 3) && radio_verify());
    }
    printf("sx1262: an apply that stops partway is not confirmed\n");

    /* The packet goes out and the return to receive does not, lost or refused: a configured radio
     * that hears nothing is not a healthy one, until it is listening again. */
    uint8_t *const back[] = {&chip.drop, &chip.refuse};
    for (size_t i = 0; i < sizeof(back) / sizeof(back[0]); i++) {
        *back[i] = CMD_SET_RX;
        assert(radio_send((const uint8_t *)"0123456789abcdefg", 17));
        assert(chip.mode != MODE_RX && !radio_verify());
        uint8_t buf[32];
        int rssi, snr;
        radio_receive(buf, sizeof(buf), 0, &rssi, &snr);   /* tries RX again on its way in */
        assert(chip.mode == MODE_RX && radio_verify());
    }
    printf("sx1262: a receiver that did not come back after sending is not reported healthy\n");

    /* A chip that reset itself holds its default sync word, not ours. */
    chip.regs[REG_LORA_SYNC_MSB] = 0x14;
    chip.regs[REG_LORA_SYNC_MSB + 1] = 0x24;
    assert(!radio_verify());
    printf("sx1262: a chip back at its defaults is noticed\n");
    return 0;
}
