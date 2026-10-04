#include "pd_status.h"
#include "board_pins.h"
#include "tasks_common.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "pd";

#define STUSB_ADDR 0x28
#define STUSB_I2C_HZ 100000
#define STUSB_TIMEOUT_MS 50

// STUSB4500 registers (ST's register map; the same ones SparkFun's library in legacy_fw reads).
#define REG_PORT_STATUS_1 0x0E // bit 0: attached
#define REG_CC_STATUS 0x11     // [1:0] CC1, [3:2] CC2: 01 default USB, 10 1.5 A, 11 3.0 A
#define REG_DEVICE_ID 0x2F
#define REG_DPM_PDO_NUMB 0x70  // [2:0] number of sink PDOs in use
#define REG_DPM_SNK_PDO1 0x85  // 3 x 4 bytes, little-endian
#define REG_RDO_STATUS 0x91    // 4 bytes: the request the chip made (the contract)
#define REG_PD_COMMAND_CTRL 0x1A
#define REG_TX_HEADER_LOW 0x51
#define PD_MSG_SOFT_RESET 0x0D // TX_HEADER_LOW: a PD soft reset (the source offers again)
#define PD_CMD_SEND 0x26       // PD_COMMAND_CTRL: send the message in TX_HEADER

// 5 V 3 A only (the user's call, 2026-10-04). The NVM (written by legacy_fw) holds sink PDO1
// 5 V 3 A and PDO2 9 V 3 A, so a PD charger that offers 9 V (most do) gives 9 V. The motor stage
// takes VBUS (9 V is within its rating, but motor_driver.c turns volts into duty against
// MOTOR_MAX_VOLTAGE_V, so the haptics would be 1.8x as strong); the LEDs have their own 5 V LDO,
// yet stayed dark on a 9 V charger -- likely the LDO's full 5.0 V output puts the WS2811 data
// threshold (~0.7 x VDD, 3.5 V) above the ESP32's 3.3 V, where from a 5 V port it sags a little
// below. So at boot, a contract above 5 V is asked again with PDO1 only:
// the chip's working copy of DPM_PDO_NUMB set to 1, then a soft reset. The NVM isn't touched; a
// power cycle loads it again, and the next boot does the same. A source that answers with a
// hard reset (power drops, the knob restarts) would loop: NVS counts the attempts in a row, and
// after PD_5V_TRIES it stops asking (logged; SYS INFO shows the voltage).
#define PD_5V_TRIES 2
#define PD_5V_SETTLE_MS 500

// A contract normally exists long before the ESP boots (the chip runs off VBUS), but give a
// freshly plugged source a moment.
#define PD_READ_TRIES 10
#define PD_READ_GAP_MS 200

// source (4 bits) | ma / 10 (10 bits) | mv / 50 (10 bits) -- one word, so any core reads it whole.
static _Atomic uint32_t s_packed = PD_SRC_READING;

static void publish(pd_source_t src, uint32_t ma, uint32_t mv) {
    uint32_t w = (uint32_t)src | ((ma / 10) & 0x3FF) << 4 | ((mv / 50) & 0x3FF) << 14;
    atomic_store_explicit(&s_packed, w, memory_order_relaxed);
}

pd_status_t pd_status_get(void) {
    uint32_t w = atomic_load_explicit(&s_packed, memory_order_relaxed);
    pd_status_t s = {
        .source = (pd_source_t)(w & 0xF),
        .ma = (uint16_t)(((w >> 4) & 0x3FF) * 10),
        .mv = (uint16_t)(((w >> 14) & 0x3FF) * 50),
    };
    return s;
}

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n) {
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, STUSB_TIMEOUT_MS);
}

static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(dev, b, sizeof(b), STUSB_TIMEOUT_MS);
}

static uint32_t le32(const uint8_t *b) {
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

// One pass over the chip. Returns true once there's nothing more to wait for (a PD contract).
static bool read_once(i2c_master_dev_handle_t dev, bool log_config) {
    uint8_t port = 0, cc = 0, rdo_b[4] = {0};
    if (rd(dev, REG_PORT_STATUS_1, &port, 1) != ESP_OK || rd(dev, REG_CC_STATUS, &cc, 1) != ESP_OK
        || rd(dev, REG_RDO_STATUS, rdo_b, 4) != ESP_OK) {
        ESP_LOGW(TAG, "STUSB4500 read failed");
        return false;
    }

    // The sink PDOs the chip is configured with (its NVM) -- only to name the contract's
    // voltage, since the RDO says which *source* PDO was taken, not its voltage.
    uint8_t npdo = 0, pdo_b[12] = {0};
    uint32_t pdo_mv[3] = {0}, pdo_ma[3] = {0};
    if (rd(dev, REG_DPM_PDO_NUMB, &npdo, 1) == ESP_OK && rd(dev, REG_DPM_SNK_PDO1, pdo_b, 12) == ESP_OK) {
        npdo &= 0x07;
        if (npdo > 3) npdo = 3;
        for (int i = 0; i < npdo; i++) {
            uint32_t p = le32(pdo_b + 4 * i);
            pdo_ma[i] = (p & 0x3FF) * 10;
            pdo_mv[i] = ((p >> 10) & 0x3FF) * 50;
            if (log_config) ESP_LOGI(TAG, "sink PDO%d: %lu mV %lu mA", i + 1, (unsigned long)pdo_mv[i], (unsigned long)pdo_ma[i]);
        }
    }

    uint32_t rdo = le32(rdo_b);
    int obj_pos = (rdo >> 28) & 0x7;
    uint32_t op_ma = ((rdo >> 10) & 0x3FF) * 10;
    int cc_state = (cc & 0x3) > ((cc >> 2) & 0x3) ? (cc & 0x3) : ((cc >> 2) & 0x3);
    ESP_LOGI(TAG, "attached=%d cc=0x%02x rdo=0x%08lx (source PDO %d, %lu mA)", port & 1, cc,
             (unsigned long)rdo, obj_pos, (unsigned long)op_ma);

    if (obj_pos != 0 && op_ma > 0) {
        // The contract's voltage: the one sink PDO asking for exactly this current. With
        // more than one match it's ambiguous -- report the current only.
        uint32_t mv = 0;
        int matches = 0;
        for (int i = 0; i < npdo; i++) {
            if (pdo_ma[i] == op_ma) {
                mv = pdo_mv[i];
                matches++;
            }
        }
        if (matches != 1) {
            // Ambiguous (this board's NVM: 5 V 3 A and 9 V 3 A ask for the same current). A
            // source's first PDO is always 5 V (USB PD), so a contract on any later one is above
            // 5 V: the one sink PDO above 5 V with this current, if there is just one.
            mv = 0;
            if (obj_pos == 1 || npdo <= 1) {
                mv = 5000;
            } else {
                matches = 0;
                for (int i = 0; i < npdo; i++) {
                    if (pdo_ma[i] == op_ma && pdo_mv[i] > 5000) {
                        mv = pdo_mv[i];
                        matches++;
                    }
                }
                if (matches != 1) mv = 0;
            }
        }
        publish(PD_SRC_PD, op_ma, mv);
        return true;
    }
    switch (cc_state) {
        case 2: publish(PD_SRC_TYPEC_1A5, 1500, 5000); break;
        case 3: publish(PD_SRC_TYPEC_3A0, 3000, 5000); break;
        default: publish(PD_SRC_USB, 500, 5000); break;
    }
    return false;
}

// The attempts in a row (NVS "pd"/"tries5v"): read, or set (-1 = read only).
static int tries_5v(int set) {
    nvs_handle_t h;
    uint8_t n = 0;
    if (nvs_open("pd", NVS_READWRITE, &h) != ESP_OK) return 0;
    if (set < 0) {
        nvs_get_u8(h, "tries5v", &n);
    } else {
        n = (uint8_t)set;
        if (nvs_set_u8(h, "tries5v", n) == ESP_OK) nvs_commit(h);
    }
    nvs_close(h);
    return n;
}

// A PD contract above 5 V: asked again with PDO1 only (see PD_5V_TRIES above).
static void ask_5v(i2c_master_dev_handle_t dev) {
    pd_status_t s = pd_status_get();
    bool above_5v = s.source == PD_SRC_PD && s.mv != 5000; // 0: not told apart, but not source PDO 1
    int tries = tries_5v(-1);
    if (!above_5v) {
        if (tries) tries_5v(0);
        return;
    }
    if (tries >= PD_5V_TRIES) {
        ESP_LOGE(TAG, "the charger keeps a %u mV contract (asked for 5 V %d times): the LEDs may stay dark and the "
                      "haptics are stronger than tuned", s.mv, tries);
        return;
    }
    tries_5v(tries + 1); // before the reset: if the power drops, the next boot knows
    uint8_t numb = 0;
    if (rd(dev, REG_DPM_PDO_NUMB, &numb, 1) != ESP_OK || wr(dev, REG_DPM_PDO_NUMB, (numb & ~0x07) | 1) != ESP_OK
        || wr(dev, REG_TX_HEADER_LOW, PD_MSG_SOFT_RESET) != ESP_OK || wr(dev, REG_PD_COMMAND_CTRL, PD_CMD_SEND) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't ask for 5 V");
        return;
    }
    ESP_LOGI(TAG, "contract at %u mV: asked again for 5 V only", s.mv);
    vTaskDelay(pdMS_TO_TICKS(PD_5V_SETTLE_MS));
    for (int i = 0; i < PD_READ_TRIES; i++) {
        if (read_once(dev, false) && pd_status_get().mv == 5000) break;
        vTaskDelay(pdMS_TO_TICKS(PD_READ_GAP_MS));
    }
    s = pd_status_get();
    if (s.mv == 5000) tries_5v(0);
    ESP_LOGI(TAG, "after asking for 5 V: source %d, %u mA @ %u mV", (int)s.source, s.ma, s.mv);
}

static void pd_task_fn(void *arg) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, // any free port
        .sda_io_num = PIN_NANO_I2C_SDA,
        .scl_io_num = PIN_NANO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, // legacy_fw (Wire) relied on these too
    };
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed");
        publish(PD_SRC_NO_CHIP, 0, 0);
        vTaskDelete(NULL);
        return;
    }
    if (i2c_master_probe(bus, STUSB_ADDR, STUSB_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "STUSB4500 not found at 0x%02x", STUSB_ADDR);
        publish(PD_SRC_NO_CHIP, 0, 0);
    } else {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = STUSB_ADDR,
            .scl_speed_hz = STUSB_I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t id = 0;
            if (rd(dev, REG_DEVICE_ID, &id, 1) == ESP_OK) ESP_LOGI(TAG, "STUSB4500 found, device id 0x%02x", id);
            for (int i = 0; i < PD_READ_TRIES; i++) {
                if (read_once(dev, i == 0)) break;
                vTaskDelay(pdMS_TO_TICKS(PD_READ_GAP_MS));
            }
            ask_5v(dev);
            i2c_master_bus_rm_device(dev);
        } else {
            publish(PD_SRC_NO_CHIP, 0, 0);
        }
    }
    i2c_del_master_bus(bus); // done: the pins go back to idle
    pd_status_t s = pd_status_get();
    ESP_LOGI(TAG, "USB power: source %d, %u mA @ %u mV", (int)s.source, s.ma, s.mv);
    vTaskDelete(NULL);
}

void pd_status_start(void) {
    // An internal stack: the task ends itself after the boot read, which frees it (a PSRAM one,
    // xTaskCreateWithCaps, would need another task to delete it).
    xTaskCreatePinnedToCore(pd_task_fn, "pd", 3072, NULL, PRIO_PD, NULL, CORE_IO);
}
