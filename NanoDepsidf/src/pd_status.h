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
