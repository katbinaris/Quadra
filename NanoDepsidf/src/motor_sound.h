#pragma once

#include <stdint.h>
#include <stdbool.h>

// Sound from the motor itself: a short decaying burst played on the detent crossing next to
// the q-axis click pulse. On the d axis it makes no torque (the windings and the magnets push
// against each other and the housing rings); on the q axis it shakes the rotor and the knob
// with it. Which carries better is one of SOUND CAL's questions. It is the device's only sound:
// the I2S amplifier and transducer the board carries are not driven (main.c holds their pins low). DEVICE -> SOUND CAL finds the frequency this knob's
// body answers to and stores it; see docs/FIRMWARE.md section 11.
//
// Plain C types only: the host UI preview (tools/ui_preview) draws the SOUND CAL screen from
// sndcal_view_t.

#define SNDCAL_TONE_COUNT 16 // log-spaced, SNDCAL_F_MIN_HZ..SNDCAL_F_MAX_HZ
#define SNDCAL_LEVEL_COUNT 4 // each tone is asked from the quietest level up
#define SNDCAL_CLICK_COUNT 8 // click shapes: each haptic profile picks one (HAPTICS -> CLICK)
#define SNDCAL_NOT_HEARD 0xFF

// The click shapes. Longer rings the body up further and reads louder; a square wave has a
// quarter more fundamental than a sine of the same voltage; a chirp lets the pitch fall to
// MOTOR_SOUND_CHIRP_END of where it starts and crosses more
// of the body's resonances than one steady pitch, which rings hollow. Lengths are times, not
// cycle counts, so a low PITCH doesn't stretch a click (a stretched chirp is a bird).
typedef struct {
    uint16_t tau_us, len_us; // the envelope's time constant, and how long it plays
    uint8_t square, chirp;
} motor_sound_shape_t;
static const motor_sound_shape_t MOTOR_SOUND_SHAPE[SNDCAL_CLICK_COUNT] = {
    {2000, 3000, 0, 0}, {4000, 6000, 0, 0}, {2000, 3000, 1, 0}, {4000, 6000, 1, 0}, {8000, 12000, 1, 0},
    {4000, 6000, 0, 1}, {4000, 6000, 1, 1}, {8000, 12000, 1, 1},
};
#define MOTOR_SOUND_CHIRP_END 0.6f
#define MOTOR_SOUND_SHAPE_DEFAULT 1 // a profile's factory click
// Names: sine or square, the time constant, CH for the chirp. The short ones are for the
// Haptics ring, where a value has six characters.
static const char *const MOTOR_SOUND_SHAPE_NAME[SNDCAL_CLICK_COUNT] = {
    "SIN 2MS", "SIN 4MS", "SQR 2MS", "SQR 4MS", "SQR 8MS", "SIN 4MS CH", "SQR 4MS CH", "SQR 8MS CH",
};
static const char *const MOTOR_SOUND_SHAPE_SHORT[SNDCAL_CLICK_COUNT] = {
    "SIN 2", "SIN 4", "SQR 2", "SQR 4", "SQR 8", "SIN 4C", "SQR 4C", "SQR 8C",
};

typedef enum {
    SNDCAL_IDLE = 0,
    SNDCAL_SWEEP,      // all the tones once on the d axis, then on the q axis, no questions
    SNDCAL_AXIS_ASK,   // which pass was louder: F1 the first (d), F3 the second (q), F2 again
    SNDCAL_TONES,      // one tone at a time: F1 heard, F3 not heard, F2 again
    SNDCAL_DONE,
    SNDCAL_MOVED,      // the knob turned while a sound played: stopped
} sndcal_stage_t;

typedef struct {
    uint8_t stage;   // sndcal_stage_t
    uint8_t step;    // 0-based, within the stage
    uint8_t level;   // 0-based, SNDCAL_TONES only
    uint8_t playing; // a sound is going out right now
    uint16_t freq_hz;
    uint8_t axis;    // what is playing or being asked: 0 the d axis, 1 the q axis
    uint8_t best;                // index of the best tone, SNDCAL_NOT_HEARD while unknown
    uint8_t heard[SNDCAL_TONE_COUNT]; // 0 not asked yet, 1..SNDCAL_LEVEL_COUNT the quietest level heard,
                                 // SNDCAL_NOT_HEARD never
    // What clicks use now (the stored calibration, or the defaults)
    uint8_t calibrated;
    uint16_t click_hz;
    uint8_t click_axis;
} sndcal_view_t;

void motor_sound_init(void); // loads the calibration, starts the calibration task

// --- Core 0, the control loop ---
// A detent click as the active haptic profile has it: PITCH (x the calibrated frequency),
// AMP (0..1) and the wave (0..SNDCAL_CLICK_COUNT-1).
void motor_sound_click(float pitch, float amp, int shape);
// The startup chime, once haptics are up: three notes, C7 E7 G7, where the motor carries. On
// the click's axis.
void motor_sound_chime(void);
// DEVICE -> CLICK: the axis (0 d, 1 q), live.
int motor_sound_axis(void);
void motor_sound_set_axis(int axis);
// This tick's voltages with the sound added, given the haptic q-axis voltage. Torque has
// priority: the sound gets what `vq` leaves inside the half-bus circle.
void motor_sound_dq(float vq, float *vd_out, float *vq_out);
// SOUND CAL is running: it owns the motor (Vq 0) and F1-F3.
bool motor_sound_cal_active(void);
void motor_sound_cal_keys(uint8_t pressed); // UI_BTN_F1..F3 press edges
void motor_sound_cal_angle(float mech_rad); // every tick while active: stops it if the knob turns
void motor_sound_cal_start(void);           // the menu's F1

// --- Core 1 ---
void motor_sound_save(void); // the frequency and the axis to NVS
// The one click shape the device had before each haptic profile got its own (menu_init()).
int motor_sound_legacy_shape(void);
void motor_sound_cal_view(sndcal_view_t *out);
