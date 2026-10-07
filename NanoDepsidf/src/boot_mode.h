#pragma once

// Phase 8 step 6: which USB personality main.c brings up at boot -- Serial (native
// USB-Serial-JTAG, TinyUSB not installed -- what holding BTN_C+BTN_D at boot has always
// forced, see main.c's usb_serial_mode_requested()) or HID (composite CDC+HID, the normal
// default). Selected via the Boot USB Mode menu screen (menu.c/menu_get_boot_mode()) and
// consumed by main.c. Shared here rather than in menu.h so config_store.c's sanity check
// doesn't need to pull in menu.c's whole navigation API just for this one enum -- same
// reasoning as haptic_params.h's haptic_type_t.
typedef enum {
    BOOT_USB_MODE_SERIAL = 0,
    BOOT_USB_MODE_HID,
    BOOT_USB_MODE_COUNT
} boot_usb_mode_t;
