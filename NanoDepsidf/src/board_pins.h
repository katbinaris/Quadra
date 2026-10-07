#pragma once

// NanoFOC_D pin mapping, ported from legacy_fw/include/nanofoc_d.h.
// Pin facts only -- no library-specific config here (e.g. no TFT_eSPI macros;
// Phase 4 configures the GC9A01 through an esp_lcd_panel_io/panel config struct instead).
// Verify against actual board revision before relying on this for bring-up.

// Motor driver (3-phase BLDC, no current-sense ADC on this board -> voltage-mode FOC only)
#define PIN_EN_U 33
#define PIN_EN_V 48
#define PIN_EN_W 36
#define PIN_IN_U 34
#define PIN_IN_V 35
#define PIN_IN_W 37

// Magnetic encoder -- MT6701, SSI mode over SPI
#define PIN_MAG_DO  21
#define PIN_MAG_CLK 18
#define PIN_MAG_CS  17

// I2C (general purpose, unused by legacy FW)
#define PIN_NANO_I2C_SDA 12
#define PIN_NANO_I2C_SCL 13

// Smart LED rings (WS2811)
#define PIN_LED_A     38
#define PIN_LED_B     42
#define NANO_LED_A_NUM 60
#define NANO_LED_B_NUM 8

// Keypad
#define PIN_BTN_A 41
#define PIN_BTN_B 40
#define PIN_BTN_C 45
#define PIN_BTN_D 46

// The MAX98357A I2S amp and its transducer: on the board, not used. Sound comes from the motor
// (motor_sound.h); main.c holds these low, so the amp sees no clock and stays shut down.
#define PIN_I2S_DOUT 9
#define PIN_I2S_BCLK 10
#define PIN_I2S_LRC  11

// Display -- GC9A01, 240x240, SPI
#define PIN_LCD_MOSI 4
#define PIN_LCD_SCLK 3
#define PIN_LCD_CS   6
#define PIN_LCD_DC   7
#define PIN_LCD_RST  2
#define PIN_LCD_BL   5   // backlight, PWM via LEDC
#define LCD_WIDTH  240
#define LCD_HEIGHT 240
#define LCD_SPI_FREQUENCY_HZ 80000000

// Secondary UART -- legacy MIDI mini-jacks. Only relevant if MIDI is kept
// (see DEVELOPMENT_PLAN.md pruning candidates -- currently assumed cut)
#define PIN_SERIAL2_RX 44
#define PIN_SERIAL2_TX 43
