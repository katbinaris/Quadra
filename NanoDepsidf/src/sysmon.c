#include "net.h"
#include "sysmon.h"
#include "sdkconfig.h"
#include "motor_config.h"
#include "mt6701.h"
#include "tasks_common.h"
#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "sys";

#define SYSMON_PERIOD_MS 500
#define SYSMON_LOG_EVERY 10 // refreshes: a log line every 5 s (only while the console isn't quiet)
#define SYSMON_QUIET_AFTER_BOOT 1 // 0 = keep INFO logging (and the 5 s line) for a serial test run

// Everything on the board that isn't the motor or the LED colours, from datasheet typicals:
// ESP32-S3 at 240 MHz on both cores with USB, no radio (~70 mA), the GC9A01 panel and its
// backlight (~30 mA), the 68 WS2811 driver ICs idling (~0.7 mA each, ~48 mA). A guess until
// the board is measured once with a USB power meter -- then set this to that reading
// (motor at rest, LEDs off) and the total becomes honest.
#define SYSMON_BOARD_MA 150.0f

// The motor stage runs from USB 5 V (motor_config.h). Phase current amplitude from the q-axis
// voltage, quasi-static (the knob turns slowly next to its electrical time constant; back-EMF
// ignored): I = vq / R. Copper loss with amplitude-invariant Clarke: P = 1.5 * vq^2 / R. Supply
// current = P / 5 V, taking the driver as lossless.
#define SYSMON_SUPPLY_V MOTOR_MAX_VOLTAGE_V

#define CPU_HZ ((float)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6f)
#define LOOP_PERIOD_CYCLES ((uint32_t)(CONTROL_LOOP_PERIOD_US * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ))
#define CONTROL_WINDOW_TICKS 1000 // Core 0 hands over its sums every 100 ms at 10 kHz

// --- Control loop (Core 0 accumulates, the sysmon task drains) ---
// Per-tick work happens in plain statics with no lock; only once per window are the sums added
// to s_ctl under a spinlock (a handful of words), which the sysmon task drains and clears.

typedef struct {
    uint32_t ticks, missed, work_max, dev_max, spikes;
    uint64_t work_sum;
    float vq2_sum;         // sum of vq^2 over the ticks
    float win_vq2_max;     // highest 100 ms window mean of vq^2
    uint64_t sec_sum[SYSMON_SEC_COUNT];
    uint32_t sec_max[SYSMON_SEC_COUNT];
} ctl_acc_t;

static ctl_acc_t s_ctl;
static portMUX_TYPE s_ctl_mux = portMUX_INITIALIZER_UNLOCKED;

// Core 0 only.
static ctl_acc_t s_win;

// Every tick, so LOOP's rate is exact: counting in 1000-tick windows made a 0.5 s sample see
// 4 or 5 windows, reading 8 or 10 kHz for a steady 10.
static _Atomic uint32_t s_ticks_total = 0;
static uint32_t s_ticks_local = 0;

void CONTROL_HOT sysmon_control_section(sysmon_section_t sec, uint32_t cycles) {
    s_win.sec_sum[sec] += cycles;
    if (cycles > s_win.sec_max[sec]) s_win.sec_max[sec] = cycles;
}

void CONTROL_HOT sysmon_control_tick(uint32_t work_cycles, uint32_t period_cycles, uint32_t notified, float vq, float vd) {
    atomic_store_explicit(&s_ticks_total, ++s_ticks_local, memory_order_relaxed);
    s_win.ticks++;
    s_win.work_sum += work_cycles;
    if (work_cycles > s_win.work_max) s_win.work_max = work_cycles;
    if (work_cycles > SYSMON_SPIKE_US * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ) s_win.spikes++;
    uint32_t dev = period_cycles > LOOP_PERIOD_CYCLES ? period_cycles - LOOP_PERIOD_CYCLES
                                                      : LOOP_PERIOD_CYCLES - period_cycles;
    if (dev > s_win.dev_max) s_win.dev_max = dev;
    if (notified > 1) s_win.missed += notified - 1;
    s_win.vq2_sum += vq * vq + vd * vd; // the motor's sound (motor_sound.h) is on the d axis
    if (s_win.ticks < CONTROL_WINDOW_TICKS) return;

    float mean = s_win.vq2_sum / s_win.ticks;
    portENTER_CRITICAL(&s_ctl_mux);
    s_ctl.ticks += s_win.ticks;
    s_ctl.missed += s_win.missed;
    s_ctl.spikes += s_win.spikes;
    s_ctl.work_sum += s_win.work_sum;
    if (s_win.work_max > s_ctl.work_max) s_ctl.work_max = s_win.work_max;
    if (s_win.dev_max > s_ctl.dev_max) s_ctl.dev_max = s_win.dev_max;
    s_ctl.vq2_sum += s_win.vq2_sum;
    if (mean > s_ctl.win_vq2_max) s_ctl.win_vq2_max = mean;
    for (int i = 0; i < SYSMON_SEC_COUNT; i++) {
        s_ctl.sec_sum[i] += s_win.sec_sum[i];
        if (s_win.sec_max[i] > s_ctl.sec_max[i]) s_ctl.sec_max[i] = s_win.sec_max[i];
    }
    portEXIT_CRITICAL(&s_ctl_mux);
    memset(&s_win, 0, sizeof(s_win));
}

// --- Other producers ---

static _Atomic uint32_t s_hid_drops = 0;
static _Atomic uint32_t s_led_ma = 0;
static _Atomic bool s_reset_request = false;

void CONTROL_HOT sysmon_note_hid_drop(void) {
    atomic_fetch_add_explicit(&s_hid_drops, 1, memory_order_relaxed);
}

void sysmon_set_led_ma(float ma) {
    atomic_store_explicit(&s_led_ma, (uint32_t)lroundf(ma), memory_order_relaxed);
}

void sysmon_reset_peaks(void) {
    atomic_store_explicit(&s_reset_request, true, memory_order_relaxed);
}

// --- Published result ---

static sysmon_info_t s_info;
static portMUX_TYPE s_info_mux = portMUX_INITIALIZER_UNLOCKED;

void sysmon_get(sysmon_info_t *out) {
    portENTER_CRITICAL(&s_info_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_info_mux);
}

// --- The task ---

static uint8_t pct(uint32_t part, uint32_t whole) {
    if (whole == 0) return 0;
    uint32_t p = (uint32_t)((uint64_t)part * 100 / whole);
    return p > 100 ? 100 : (uint8_t)p;
}

// Where the internal RAM goes: each heap's free / lowest / largest block, and every task's
// unused stack (its high-water mark) and where the stack lives. The SYS INFO "heap" is the
// internal line. The task table is allocated in PSRAM, not on this small stack.
static void memory_report(void) {
    static const struct {
        const char *name;
        uint32_t caps;
    } heaps[] = {{"internal", MALLOC_CAP_INTERNAL}, {"internal dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA}, {"psram", MALLOC_CAP_SPIRAM}};
    for (size_t i = 0; i < sizeof(heaps) / sizeof(heaps[0]); i++) {
        ESP_LOGI(TAG, "memory %-12s free %7u  lowest %7u  largest block %7u", heaps[i].name,
                 (unsigned)heap_caps_get_free_size(heaps[i].caps), (unsigned)heap_caps_get_minimum_free_size(heaps[i].caps),
                 (unsigned)heap_caps_get_largest_free_block(heaps[i].caps));
    }
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (t == NULL) return;
    n = uxTaskGetSystemState(t, n, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "task %-16s stack unused %5u B  in %s", t[i].pcTaskName, (unsigned)t[i].usStackHighWaterMark,
                 esp_ptr_external_ram(t[i].pxStackBase) ? "psram" : "internal");
    }
    heap_caps_free(t);
}

static void sysmon_task_fn(void *arg) {
    temperature_sensor_handle_t tsens = NULL;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &tsens) != ESP_OK || temperature_sensor_enable(tsens) != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor unavailable");
        tsens = NULL;
    }

    sysmon_info_t info = {0};
    info.chip_peak_c = -100.0f;
    uint32_t hid_base = 0, crc_base = 0;
    uint32_t ticks_prev = atomic_load_explicit(&s_ticks_total, memory_order_relaxed);
    uint32_t idle_prev[2] = {ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)};
    int64_t t_prev = esp_timer_get_time();
    uint32_t n = 0;
    TickType_t wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(SYSMON_PERIOD_MS));
        int64_t now = esp_timer_get_time();
        uint32_t elapsed_us = (uint32_t)(now - t_prev);
        t_prev = now;

        bool reset = atomic_exchange_explicit(&s_reset_request, false, memory_order_relaxed);
        uint32_t hid_now = atomic_load_explicit(&s_hid_drops, memory_order_relaxed);
        uint32_t crc_now = mt6701_crc_errors();
        if (reset) {
            hid_base = hid_now;
            crc_base = crc_now;
            info.total_peak_ma = 0;
            info.chip_peak_c = -100.0f;
            info.coil_peak_ma = 0;
            info.load_peak[0] = info.load_peak[1] = 0;
            info.work_max_us = info.jitter_max_us = 0;
            info.missed = 0;
            memset(info.sec_max_us, 0, sizeof(info.sec_max_us));
        }

        ctl_acc_t c;
        portENTER_CRITICAL(&s_ctl_mux);
        c = s_ctl;
        memset(&s_ctl, 0, sizeof(s_ctl));
        portEXIT_CRITICAL(&s_ctl_mux);

        // Control loop
        uint32_t ticks_total = atomic_load_explicit(&s_ticks_total, memory_order_relaxed);
        info.loop_khz = (ticks_total - ticks_prev) / (elapsed_us / 1000.0f);
        ticks_prev = ticks_total;
        if (c.ticks > 0) {
            info.work_avg_us = (float)c.work_sum / c.ticks / (CPU_HZ / 1e6f);
            float mean_vq2 = c.vq2_sum / c.ticks;
            info.coil_ma = (uint16_t)lroundf(sqrtf(mean_vq2) / MOTOR_PHASE_RESISTANCE_OHM * 1000.0f);
            info.copper_w = 1.5f * mean_vq2 / MOTOR_PHASE_RESISTANCE_OHM;
            info.motor_ma = (uint16_t)lroundf(info.copper_w / SYSMON_SUPPLY_V * 1000.0f);
            uint16_t win_peak = (uint16_t)lroundf(sqrtf(c.win_vq2_max) / MOTOR_PHASE_RESISTANCE_OHM * 1000.0f);
            if (win_peak > info.coil_peak_ma) info.coil_peak_ma = win_peak;
            float parts = 0;
            for (int i = 0; i < SYSMON_SEC_COUNT; i++) {
                info.sec_avg_us[i] = (float)c.sec_sum[i] / c.ticks / (CPU_HZ / 1e6f);
                parts += info.sec_avg_us[i];
                float mx = c.sec_max[i] / (CPU_HZ / 1e6f);
                if (mx > info.sec_max_us[i]) info.sec_max_us[i] = mx;
            }
            info.other_avg_us = info.work_avg_us > parts ? info.work_avg_us - parts : 0;
        }
        float work_max_us = c.work_max / (CPU_HZ / 1e6f), jitter_us = c.dev_max / (CPU_HZ / 1e6f);
        if (work_max_us > info.work_max_us) info.work_max_us = work_max_us;
        if (jitter_us > info.jitter_max_us) info.jitter_max_us = jitter_us;
        info.missed += c.missed;
        info.spikes_per_s = c.spikes / (elapsed_us / 1e6f);

        // Core load: whatever the idle task didn't get.
        for (int core = 0; core < 2; core++) {
            uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
            uint32_t idle_us = idle - idle_prev[core];
            idle_prev[core] = idle;
            info.load[core] = 100 - pct(idle_us, elapsed_us);
            if (info.load[core] > info.load_peak[core]) info.load_peak[core] = info.load[core];
        }

        // Heat
        float t;
        info.chip_ok = tsens != NULL && temperature_sensor_get_celsius(tsens, &t) == ESP_OK;
        if (info.chip_ok) {
            info.chip_c = t;
            if (t > info.chip_peak_c) info.chip_peak_c = t;
        }

        // Power
        info.led_ma = (uint16_t)atomic_load_explicit(&s_led_ma, memory_order_relaxed);
        info.board_ma = (uint16_t)(SYSMON_BOARD_MA + net_current_ma()); // + the WiFi radio, while it's on
        uint32_t total = (uint32_t)info.motor_ma + info.led_ma + info.board_ma;
        info.total_ma = total > 0xFFFF ? 0xFFFF : (uint16_t)total;
        if (info.total_ma > info.total_peak_ma) info.total_peak_ma = info.total_ma;

        // System
        info.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        info.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        info.hid_drops = hid_now - hid_base;
        info.sensor_crc_errors = crc_now - crc_base;
        info.uptime_s = (uint32_t)(now / 1000000);
        info.version++;

        portENTER_CRITICAL(&s_info_mux);
        s_info = info;
        portEXIT_CRITICAL(&s_info_mux);

        // Once, 10 s in (every driver installed by then): which interrupt sits on which core --
        // anything on CPU 0 besides the pacing gptimer, the esp_timer and the tick competes
        // with the control loop.
        if (n + 1 == 20) {
            ESP_LOGI(TAG, "interrupt allocation:");
            esp_intr_dump(NULL);
            memory_report();
#if SYSMON_QUIET_AFTER_BOOT
            // From here on only warnings and errors reach the console: nobody reads it in
            // normal use, and a console write with no reader can block the writing task.
            ESP_LOGI(TAG, "boot done, console quiet from now on (warnings and errors only)");
            esp_log_level_set("*", ESP_LOG_WARN);
#endif
        }
        // Again a minute in, with WiFi (and a companion, if one's in) up: this one past the
        // quiet console, for this tag only.
        if (n + 1 == 120) {
            esp_log_level_set(TAG, ESP_LOG_INFO);
            memory_report();
#if SYSMON_QUIET_AFTER_BOOT
            esp_log_level_set(TAG, ESP_LOG_WARN);
#endif
        }
        if (++n % SYSMON_LOG_EVERY == 0) {
            ESP_LOGI(TAG, "load %u/%u%% | loop %.2f kHz work %.1f/%.1f us jitter %.1f us missed %lu spikes %.0f/s | "
                          "chip %.1f C | est %u mA (motor %u, led %u, board %u) coil %u mA | "
                          "heap %lu/%lu | hid drops %lu | "
                          "input %.1f sensor %.1f force %.1f motor %.1f other %.1f us, sensor crc errors %lu",
                     info.load[0], info.load[1], info.loop_khz, info.work_avg_us, info.work_max_us,
                     info.jitter_max_us, (unsigned long)info.missed, info.spikes_per_s, info.chip_c, info.total_ma, info.motor_ma,
                     info.led_ma, info.board_ma, info.coil_ma, (unsigned long)info.heap_free,
                     (unsigned long)info.heap_min, (unsigned long)info.hid_drops,
                     info.sec_avg_us[SYSMON_SEC_INPUT], info.sec_avg_us[SYSMON_SEC_SENSOR],
                     info.sec_avg_us[SYSMON_SEC_FORCE], info.sec_avg_us[SYSMON_SEC_MOTOR], info.other_avg_us,
                     (unsigned long)info.sensor_crc_errors);
        }
    }
}

void sysmon_start(void) {
    // Stack in PSRAM (tasks_common.h: PSRAM stacks)
    xTaskCreatePinnedToCoreWithCaps(sysmon_task_fn, "sysmon", 3072, NULL, PRIO_SYSMON, NULL, CORE_IO, MALLOC_CAP_SPIRAM);
}
