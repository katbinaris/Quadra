#include "icon_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "icon";

// Two buffers: the transfer assembles into s_staging (only ever touched from the TinyUSB
// task), and only a length+CRC-verified image is copied into s_committed under the mutex --
// the display can never see a half-received icon.
// In PSRAM (EXT_RAM_BSS_ATTR): touched only on an upload and an icon copy, never per pixel.
EXT_RAM_BSS_ATTR static uint8_t s_staging[ICON_BYTES];
EXT_RAM_BSS_ATTR static uint8_t s_committed[ICON_BYTES];
static bool s_has_icon = false;
static volatile uint32_t s_version = 0;
static SemaphoreHandle_t s_lock = NULL;

// Transfer state -- TinyUSB task only.
static bool s_rx_active = false;
static uint16_t s_rx_expected = 0;
static uint16_t s_rx_received = 0;
static uint32_t s_rx_crc = 0;

// Bitwise CRC-32 (IEEE, reflected, poly 0xEDB88320) -- matches zlib.crc32. A few thousand
// bytes once per upload, so no table needed.
static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool reply(uint8_t *out, uint8_t cmd, icon_status_t status) {
    memset(out, 0, ICON_HID_REPORT_SIZE);
    out[0] = ICON_REPLY_TAG;
    out[1] = cmd;
    out[2] = (uint8_t)status;
    out[3] = (uint8_t)(s_rx_received & 0xFF);
    out[4] = (uint8_t)(s_rx_received >> 8);
    return true;
}

static bool fail(uint8_t *out, uint8_t cmd, icon_status_t status) {
    ESP_LOGW(TAG, "cmd 0x%02x failed: status %d (received %u/%u)", cmd, status, s_rx_received, s_rx_expected);
    s_rx_active = false;
    return reply(out, cmd, status);
}

void icon_store_init(void) {
    s_lock = xSemaphoreCreateMutex();
}

bool icon_store_handle_report(const uint8_t *report, uint16_t len, uint8_t *out) {
    if (len < 1) {
        return false;
    }
    const uint8_t cmd = report[0];

    switch (cmd) {
        case ICON_CMD_BEGIN: {
            if (len < 10) {
                return fail(out, cmd, ICON_ST_BAD_PARAM);
            }
            const uint8_t w = report[1], h = report[2], fmt = report[3];
            const uint16_t total = rd_u16(&report[4]);
            if (w != ICON_WIDTH || h != ICON_HEIGHT || fmt != ICON_FORMAT_RGB565_BE || total != ICON_BYTES) {
                s_rx_received = 0;
                return fail(out, cmd, ICON_ST_BAD_PARAM);
            }
            s_rx_active = true;
            s_rx_expected = total;
            s_rx_received = 0;
            s_rx_crc = rd_u32(&report[6]);
            ESP_LOGI(TAG, "upload begin: %ux%u, %u bytes", w, h, total);
            return reply(out, cmd, ICON_ST_OK);
        }

        case ICON_CMD_DATA: {
            if (!s_rx_active) {
                return fail(out, cmd, ICON_ST_BAD_STATE);
            }
            const uint16_t offset = rd_u16(&report[1]);
            const uint8_t n = report[3];
            if (len < ICON_DATA_HEADER + n || n == 0 || n > ICON_DATA_MAX) {
                return fail(out, cmd, ICON_ST_BAD_PARAM);
            }
            if (offset != s_rx_received || (uint32_t)offset + n > s_rx_expected) {
                return fail(out, cmd, ICON_ST_BAD_OFFSET);
            }
            memcpy(&s_staging[offset], &report[ICON_DATA_HEADER], n);
            s_rx_received += n;
            return false; // no reply per chunk -- only failures answer
        }

        case ICON_CMD_END: {
            if (!s_rx_active) {
                return fail(out, cmd, ICON_ST_BAD_STATE);
            }
            if (s_rx_received != s_rx_expected) {
                return fail(out, cmd, ICON_ST_INCOMPLETE);
            }
            if (crc32_ieee(s_staging, s_rx_expected) != s_rx_crc) {
                return fail(out, cmd, ICON_ST_CRC_FAIL);
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_committed, s_staging, ICON_BYTES);
            s_has_icon = true;
            s_version++;
            xSemaphoreGive(s_lock);
            s_rx_active = false;
            ESP_LOGI(TAG, "upload committed (%u bytes, crc ok)", s_rx_expected);
            return reply(out, cmd, ICON_ST_OK);
        }

        case ICON_CMD_CLEAR: {
            s_rx_active = false;
            s_rx_received = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_has_icon = false;
            s_version++;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "icon cleared");
            return reply(out, cmd, ICON_ST_OK);
        }

        default:
            return fail(out, cmd, ICON_ST_BAD_PARAM);
    }
}

uint32_t icon_store_version(void) {
    return s_version;
}

bool icon_store_copy(uint8_t *dst, size_t dst_len) {
    if (dst_len < ICON_BYTES) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_has_icon;
    if (has) {
        memcpy(dst, s_committed, ICON_BYTES);
    }
    xSemaphoreGive(s_lock);
    return has;
}
