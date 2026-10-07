#include "mt6701.h"
#include "tasks_common.h"
#include "board_pins.h"
#include "driver/spi_master.h"
#include "soc/spi_struct.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdatomic.h>

static const char *TAG = "mt6701";
static spi_device_handle_t s_spi;

// SSI clock. Was a conservative 1 MHz from first bring-up: SYS INFO's LOOP page then showed the
// read taking 41 of the control loop's 55 us (24 us of it clocking 24 bits). The MT6701's SSI
// allows up to 15.625 MHz. Watch the CRC error count (LOOP page) after changing this.
#define MT6701_SPI_HZ (10 * 1000 * 1000)

// Frames whose CRC didn't match, since boot. Counted only: the angle is still used either way,
// until the check is confirmed on hardware (a wrong CRC formula would otherwise freeze the knob).
static _Atomic uint32_t s_crc_errors = 0;
// Reads go straight to the SPI2 registers, not through the driver. Set by mt6701_init() once
// direct reads have matched the driver's; Core 0 only after that.
static bool s_direct = false;
#define DIRECT_CHECKS 8
#define DIRECT_SPIN_MAX 4000 // far past the 2.4 us a read takes; only a broken bus gets here
#define DIRECT_CHECK_COUNTS 200 // of 16384: 4.4 degrees between two reads a few microseconds apart
// For mt6701_report(): how the self-check went and what a read costs. Plain words, Core 0
// writes them; the report reads them once, so a torn value would only be a wrong number in a log.
static struct {
    uint8_t pass, why;
    uint32_t a, b;
    uint32_t driver_cycles, direct_cycles;
} s_diag;
static uint32_t s_latch_spins, s_done_spins; // the last direct read's two waits, in loop turns

static bool read_driver(uint32_t *raw24);
static bool read_direct(uint32_t *raw24);
static uint8_t crc6(uint32_t data18);

esp_err_t mt6701_init(void) {
    // Encoder gets its own SPI host (SPI2), separate from the display's (SPI3, Phase 4) --
    // decided in DEVELOPMENT_PLAN.md to avoid any bus contention between the control loop's
    // sensor reads and display DMA bursts.
    spi_bus_config_t buscfg = {
        .mosi_io_num = -1, // sensor only outputs data, never reads command bytes
        .miso_io_num = PIN_MAG_DO,
        .sclk_io_num = PIN_MAG_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    // MT6701 SSI is not register-addressed SPI -- it's a continuous serial output, just a
    // fixed-width clocked frame. SPI mode 0, MSB first.
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = MT6701_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_MAG_CS,
        .queue_size = 1,
        .flags = 0,
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    // The sensor is the bus's only device and only the control task reads it (mt6701_init()
    // runs in that task): hold the bus for good, so each read skips the driver's bus lock.
    err = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi_device_acquire_bus failed: %s (reads still work, a little slower)", esp_err_to_name(err));
        return ESP_OK; // and through the driver only: without the bus the registers aren't ours to keep
    }

    // Direct reads (read_direct()) only once they have proved themselves here: each must pass
    // the CRC and agree with the driver's read just before it. An all-zero frame passes the CRC
    // too, hence the comparison. The knob may be turning, so a few counts apart is fine.
    // What happened is kept for mt6701_report(): this runs before the console is listening.
    bool ok = true;
    for (int i = 0; i < DIRECT_CHECKS && ok; i++) {
        uint32_t a = 0, b = 0;
        uint32_t t0 = esp_cpu_get_cycle_count();
        bool da = read_driver(&a);
        uint32_t t1 = esp_cpu_get_cycle_count();
        bool db = read_direct(&b);
        uint32_t t2 = esp_cpu_get_cycle_count();
        s_diag.pass = i;
        s_diag.a = a;
        s_diag.b = b;
        s_diag.driver_cycles = t1 - t0;
        s_diag.direct_cycles = t2 - t1;
        int d = (int)((a >> 10) & 0x3FFF) - (int)((b >> 10) & 0x3FFF);
        if (d > 8192) d -= 16384;
        if (d < -8192) d += 16384;
        s_diag.why = !da ? 1 : !db ? 2 : crc6(a >> 6) != (a & 0x3F) ? 3 : crc6(b >> 6) != (b & 0x3F) ? 4
                   : (d <= -DIRECT_CHECK_COUNTS || d >= DIRECT_CHECK_COUNTS) ? 5 : 0;
        ok = s_diag.why == 0;
    }
    s_direct = ok;
    return ESP_OK;
}

void mt6701_report(void) {
    static const char *const why[] = {"ok", "the driver's read failed", "the direct read timed out", "the driver's read failed its CRC",
                                      "the direct read failed its CRC", "the two reads disagree"};
    const float us = 1.0f / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    ESP_LOGI(TAG, "reads: %s (self-check pass %u of %d: %s; driver frame %06lx, direct frame %06lx)",
             s_direct ? "direct, on the SPI2 registers" : "through the SPI driver", s_diag.pass + 1, DIRECT_CHECKS, why[s_diag.why],
             (unsigned long)s_diag.a, (unsigned long)s_diag.b);
    ESP_LOGI(TAG, "one read at boot: driver %.1f us, direct %.1f us; last read waited %lu + %lu spins",
             s_diag.driver_cycles * us, s_diag.direct_cycles * us, (unsigned long)s_latch_spins,
             (unsigned long)s_done_spins);
}

// MT6701 SSI CRC: X^6 + X + 1 over the 18 data bits (14 angle + 4 status), MSB first, init 0.
static uint8_t CONTROL_HOT crc6(uint32_t data18) {
    uint8_t crc = 0;
    for (int i = 17; i >= 0; i--) {
        uint8_t bit = ((data18 >> i) & 1) ^ ((crc >> 5) & 1);
        crc = (crc << 1) & 0x3F;
        if (bit) crc ^= 0x03;
    }
    return crc;
}

uint32_t mt6701_crc_errors(void) {
    return atomic_load_explicit(&s_crc_errors, memory_order_relaxed);
}

// One read through ESP-IDF's driver: about 13 us, of which the 24 bits take 2.4. The rest is
// the driver setting the whole transaction up again. False if the driver refused.
static bool CONTROL_HOT read_driver(uint32_t *raw24) {
    uint8_t rx[3] = {0};
    spi_transaction_t t = {
        .length = 24,
        .rxlength = 24,
        .tx_buffer = NULL,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi read failed: %s", esp_err_to_name(err));
        return false;
    }
    *raw24 = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    return true;
}

// The same read on the SPI2 registers themselves. Every read is the same transaction, the
// sensor is alone on SPI2 and the bus stays acquired (mt6701_init()), so the registers the
// driver programmed for its last read are still right: clear the done flag, start, wait for
// the 24 bits, take the word. No interrupt fires: the driver keeps SPI2's switched off while
// a device holds the bus. False if the transfer never finished.
static bool CONTROL_HOT read_direct(uint32_t *raw24) {
    spi_dev_t *hw = &GPSPI2;
    hw->dma_int_clr.trans_done = 1;
    // As the driver does before every start: latch the configuration into the SPI clock's
    // side. Nothing has changed, so it is a formality, and a fraction of a microsecond.
    hw->cmd.update = 1;
    int latch = 0;
    while (hw->cmd.update && latch < DIRECT_SPIN_MAX) latch++;
    s_latch_spins = latch;
    hw->cmd.usr = 1;
    for (int i = 0; i < DIRECT_SPIN_MAX; i++) {
        if (hw->dma_int_raw.trans_done) {
            s_done_spins = i;
            uint32_t w = hw->data_buf[0]; // the first byte in is the low byte
            *raw24 = ((w & 0xFF) << 16) | (w & 0xFF00) | ((w >> 16) & 0xFF);
            return true;
        }
    }
    return false;
}

int32_t CONTROL_HOT mt6701_read_angle_raw(void) {
    uint32_t raw24;
    if (s_direct) {
        if (!read_direct(&raw24)) {
            s_direct = false; // back to the driver for good; this read is lost
            ESP_LOGE(TAG, "direct read timed out: using the SPI driver from here on");
            return -1;
        }
    } else if (!read_driver(&raw24)) {
        return -1;
    }

    // The 24-bit SSI frame: angle [23:10], status [9:6], CRC [5:0].
    if (crc6(raw24 >> 6) != (raw24 & 0x3F)) {
        atomic_fetch_add_explicit(&s_crc_errors, 1, memory_order_relaxed);
    }
    uint16_t angle14 = (uint16_t)((raw24 >> 10) & 0x3FFF);
    return angle14;
}

float mt6701_read_angle_rad(void) {
    int32_t raw = mt6701_read_angle_raw();
    if (raw < 0) {
        return NAN;
    }
    return ((float)raw / 16384.0f) * (2.0f * (float)M_PI);
}
