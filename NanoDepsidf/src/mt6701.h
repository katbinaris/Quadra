#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t mt6701_init(void);

// Raw 14-bit angle (0-16383), or -1 on read failure.
int32_t mt6701_read_angle_raw(void);

// Angle in radians [0, 2*PI). NAN on read failure.
float mt6701_read_angle_rad(void);

// Frames whose SSI CRC didn't match, since boot (SYS INFO). Any core.
uint32_t mt6701_crc_errors(void);

// Logs how reads are made (directly on the SPI registers, or through the driver and why) and
// what one costs. sysmon.c calls it ten seconds in, once the console has a reader. Core 1.
void mt6701_report(void);
