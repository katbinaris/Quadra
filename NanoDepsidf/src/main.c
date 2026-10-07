#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "board_pins.h"
#include "ipc.h"
#include "motor_sound.h"
#include "ui_state.h"
#include "menu.h"
#include "app_profiles/app_profiles.h"
#include "icon_store.h"
#include "control_task.h"
#include "usb_task.h"
#include "display_task.h"
#include "led_task.h"
#include "pd_status.h"
#include "sysmon.h"
#include "ext_link.h"
#include "net.h"
#include "home.h"
#include "midi.h"
#include "net_link.h"
#include "clock.h"
#include "hal/usb_serial_jtag_ll.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "nanod";

// Phase 3 USB-mode select, checked once here before any task starts (not folded into
// control_task.c's own BTN_A-prefixed arm-window logic -- this is a different, unrelated
// decision: which USB personality to bring up, not which motor/haptic diagnostic to run).
//
// Our composite TinyUSB device (CDC console + HID keyboard/mouse/gamepad, usb_task.c)
// claims the chip's one physical USB-OTG PHY away from the built-in USB-Serial-JTAG
// peripheral, which is what transparently handled console + `pio run -t upload`'s
// 1200bps-touch reset-to-bootloader trick throughout Phases 0-2 with zero app code at
// all (CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG, sdkconfig.esp32-s3-devkitm-1) --
// TinyUSB CDC-ACM doesn't get that reset trick for free (there's no
// tud_cdc_line_coding_cb() implementing it), and porting the register-level PHY-mux
// dance ESP32-Arduino's usb_persist_restart() uses to do it manually is real, hard-to-
// verify-without-hardware chip-specific complexity -- see the DEVELOPMENT_PLAN.md Phase 3
// entry for that history. Sidestepping it entirely: hold BTN_C+BTN_D at boot to skip
// installing TinyUSB altogether, leaving the PHY on its default USB-Serial-JTAG routing
// (confirmed default from every prior phase's bring-up) -- normal flashing/console just
// works, exactly as it always did, at the cost of HID being unavailable for that boot.
// Release both (normal boot) to get the composite HID+CDC device instead.
#define USB_MODE_SELECT_POLL_ITERS 40 // 40 x 50ms = 2s -- long enough to reliably catch a
                                      // held combo without needing exact boot-instant
                                      // timing (same lesson control_task.c's own BTN_A
                                      // arm-window comment documents)

static bool usb_serial_mode_requested(void) {
    gpio_config_t btn_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BTN_C) | (1ULL << PIN_BTN_D),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);

    for (int i = 0; i < USB_MODE_SELECT_POLL_ITERS; i++) {
        bool c_pressed = (gpio_get_level(PIN_BTN_C) == 0); // active-low, pull-up
        bool d_pressed = (gpio_get_level(PIN_BTN_D) == 0);
        if (c_pressed && d_pressed) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SW (esp_restart)";
        case ESP_RST_PANIC:     return "PANIC (crash)";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_USB:       return "USB";
        default:                return "OTHER/UNKNOWN";
    }
}

void app_main(void) {
    // Logged unconditionally at every boot -- became important during Phase 2a motor
    // bring-up, where the board reset repeatedly under motor load and we needed hard
    // evidence of *why* (brownout vs. crash vs. watchdog) instead of guessing.
    ESP_LOGW(TAG, "Reset reason: %s", reset_reason_str(esp_reset_reason()));

    ESP_LOGI(TAG, "Nano_D++ ESP-IDF bring-up, LCD_CS=%d, MAG_CS=%d", PIN_LCD_CS, PIN_MAG_CS);

    // Standard erase-and-retry pattern: the NVS partition can be reported as needing an
    // erase after a truncated write or a partition layout change (e.g. from earlier
    // bring-up when the partition table itself was being iterated on).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s) -- erasing and retrying", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ipc_init();
    motor_sound_init(); // after nvs_flash_init(): it loads SOUND CAL's result
    // The I2S amp isn't driven any more (board_pins.h): its inputs low, not floating.
    const gpio_config_t amp_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_I2S_DOUT) | (1ULL << PIN_I2S_BCLK) | (1ULL << PIN_I2S_LRC),
    };
    gpio_config(&amp_cfg);
    gpio_set_level(PIN_I2S_DOUT, 0);
    gpio_set_level(PIN_I2S_BCLK, 0);
    gpio_set_level(PIN_I2S_LRC, 0);
    ui_state_init();
    clock_init();
    app_profiles_init(); // stored profiles: before menu_init() looks up the saved one by id
    midi_synths_init();  // the synth profiles (LittleFS is mounted now): before menu_init() too
    menu_init();
    icon_store_init(); // before usb_task (producer) and display_task (consumer) start

    // See usb_serial_mode_requested()'s comment above -- checked before any task starts.
    // Phase 8 step 6: the BTN_C+BTN_D hold is a hardware failsafe that ALWAYS takes priority,
    // regardless of the saved Boot USB Mode setting below -- it exists specifically so a
    // saved "HID" preference (which drops the native USB-Serial-JTAG console/flashing path,
    // see that function's comment) can never make this board unrecoverable without a
    // physical BOOT-button reflash. menu_get_boot_mode() reads the value menu_init() already
    // restored from NVS above (defaults to HID, matching this board's prior unconditional
    // behavior, if nothing was ever saved).
    // A host-requested serial boot (ext_proto.h EXT_REBOOT_SERIAL, for flashing) skips the
    // 2 s button poll -- nobody is holding anything.
    bool serial_requested_by_host = ext_take_serial_boot();
    bool serial_forced_by_buttons = !serial_requested_by_host && usb_serial_mode_requested();
    bool serial_requested_by_setting = (menu_get_boot_mode() == BOOT_USB_MODE_SERIAL);
    bool usb_serial_mode = serial_forced_by_buttons || serial_requested_by_setting || serial_requested_by_host;
    ui_state_set_usb_serial_active(usb_serial_mode); // what the UI shows as "IN USE"
    if (usb_serial_mode) {
        // The internal PHY's mux (USB-OTG vs USB-Serial-JTAG) is an RTC_CNTL register, so a
        // soft restart out of HID mode can leave it on USB-OTG. Hand it to USB-Serial-JTAG
        // explicitly or its port never enumerates. Same state as a power-on, so harmless then.
        usb_serial_jtag_ll_phy_enable_external(false);
        usb_serial_jtag_ll_phy_disable_pull_override();
        usb_serial_jtag_ll_phy_enable_pad(true);
    }
    if (serial_requested_by_host) {
        ESP_LOGW(TAG, "Serial boot requested by the host (EXT_REBOOT_SERIAL): TinyUSB (HID) "
                       "will NOT be installed this boot -- flash now; the next reset boots normally.");
    } else if (serial_forced_by_buttons) {
        ESP_LOGW(TAG, "BTN_C+BTN_D held at boot -- USB serial mode (failsafe, overrides the "
                       "saved Boot USB Mode setting): TinyUSB (HID) will NOT be installed "
                       "this boot. Native USB-Serial-JTAG console/flashing stays on its "
                       "default routing -- flash normally now.");
    } else if (serial_requested_by_setting) {
        ESP_LOGI(TAG, "Boot USB Mode setting is Serial -- TinyUSB (HID) will NOT be "
                       "installed this boot. Hold BTN_C+BTN_D at boot at any time to force "
                       "this regardless of the saved setting.");
    }

    // Core 0: control loop, kept exclusive per DEVELOPMENT_PLAN.md
    control_task_start();

    // Before USB and WiFi take their share of internal RAM: a PSRAM frame halves the frame rate.
    display_frame_reserve();

    // Core 1: everything DMA-offloaded/tolerant
    midi_start(); // MIDI mode (midi.h): the TRS jacks work in either USB mode
    if (!usb_serial_mode) {
        net_start(); // before the usb task: a NET setup arriving over USB finds it running
        home_start(); // HOME's lamps (home.h); likewise before an import can arrive
        usb_task_start();
        net_link_start(); // the companion over WiFi, through the usb task's host_link
    } else {
        ESP_LOGI(TAG, "usb task not started (USB serial mode active this boot)");
    }
    display_task_start();
    led_task_start();
    pd_status_start(); // one-shot STUSB4500 read (SYS INFO shows it); a 9 V contract is asked again for 5 V
    sysmon_start();    // SYS INFO: load, loop timing, heat, estimated power
}
