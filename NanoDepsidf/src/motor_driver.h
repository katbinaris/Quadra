#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t motor_driver_init(void);
void motor_driver_enable(bool enable);

// Phase voltages in volts, referenced to a virtual neutral at MOTOR_MAX_VOLTAGE_V/2.
// Internally clamped to the physical supply rail -- does NOT enforce the current-derived
// safety limit itself, callers must clamp Vd/Vq to MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V or
// MOTOR_EFFECTIVE_ROTATING_VOLTAGE_LIMIT_V (whichever applies) first.
void motor_driver_set_phase_voltages(float ua, float ub, float uc);

// The same with sound on top, stepped at the PWM rate (32 kHz), not the loop's: up to
// MOTOR_TONE_VOICES voices, each a sine, a square or noise, on the d or the q axis. Call it every tick for as
// long as anything sounds (everything in it may change from tick to tick);
// motor_driver_set_phase_voltages() ends it. Core 0 only.
#define MOTOR_TONE_VOICES 5
#define MOTOR_TONE_SINE 0
#define MOTOR_TONE_SQUARE 1
#define MOTOR_TONE_NOISE 2 // cycles of a sine at `hz`, each upside down or not at random: no DC in it
#define MOTOR_TONE_RICH 3  // a sine with its second and third harmonics, at 1/2 and 1/4: `hz` under 3300
typedef struct {
    float hz;    // under 16000
    float volts; // peak; 0 = silent
    bool q;      // on the q axis, not the d axis
    uint8_t wave; // MOTOR_TONE_SINE..
    bool start;  // this tick starts it: from phase 0, or from the peak with `cosine`
    bool cosine;
} motor_tone_voice_t;
typedef struct motor_tone {
    float u[3];               // the phase voltages without the sound
    float d[3], q[3];         // 1 V on the d and on the q axis, on each phase
    float d_lim, q_min, q_max; // the summed sound is clipped to +-d_lim and q_min..q_max, in volts
    motor_tone_voice_t voice[MOTOR_TONE_VOICES];
} motor_tone_t;
void motor_driver_tone(const motor_tone_t *tone);
