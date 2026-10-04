#pragma once

#include <stdbool.h>
#include <stdint.h>

// USB power, as the STUSB4500 (the board's USB-C / PD sink, I2C 0x28 on SDA 12 / SCL 13)
// negotiated it on its own. Read once, briefly, at boot. One write: a contract above 5 V (a PD
// charger, from the NVM's 9 V PDO) is asked again for 5 V only -- the chip's working copy, then a
// soft reset (pd_status.c, PD_5V_TRIES). Its NVM is never written.

typedef enum {
    PD_SRC_READING = 0, // boot read not finished yet
    PD_SRC_NO_CHIP,     // the STUSB4500 didn't answer on I2C
    PD_SRC_USB,         // no USB-C current advertised: plain USB, 500 mA (what we ask the host for)
    PD_SRC_TYPEC_1A5,   // USB-C Rp current, no PD contract
    PD_SRC_TYPEC_3A0,
    PD_SRC_PD,          // PD contract
} pd_source_t;

typedef struct {
    pd_source_t source;
    uint16_t ma; // available current; 0 while reading / no chip
    uint16_t mv; // PD voltage when it can be told from the sink PDOs, else 5000 (or 0: unknown)
} pd_status_t;

// Starts a short-lived low-priority task that reads the chip (retrying for a moment while a
// contract is still being made), stores the result and exits.
void pd_status_start(void);

// Any core, any time: the last result (PD_SRC_READING until the boot read is done).
pd_status_t pd_status_get(void);

// The chip's NVM, for good: sink PDO1 only (5 V 3 A), so even before the firmware runs (and in
// the ROM bootloader) it never asks for more. One-time, from quadra.py pd --write-5v
// (EXT_CMD_PD, USB only); the usb task, ~100 ms. Reads the NVM into `before`, checks its layout
// against the chip's working copy, and only then (with `write`) erases, writes it back with the
// PDO count 1 and reads it back to compare (twice at most). `write` false: read and check only.
#define PD_NVM_SECTORS 5
#define PD_NVM_BYTES (PD_NVM_SECTORS * 8)
typedef enum {
    PD_NVM_OK = 0,          // written and verified (or, read only: it would write)
    PD_NVM_ALREADY,         // already PDO1 only: nothing written
    PD_NVM_BUSY,            // the boot read still has the bus: try again in a few seconds
    PD_NVM_NO_CHIP,
    PD_NVM_READ_FAILED,
    PD_NVM_UNEXPECTED,      // its contents don't match the working copy: nothing written
    PD_NVM_VERIFY_FAILED,   // written twice, read back different both times
} pd_nvm_result_t;
pd_nvm_result_t pd_nvm_5v(bool write, uint8_t before[PD_NVM_BYTES], uint8_t *pdos_before, uint8_t *pdos_after);
