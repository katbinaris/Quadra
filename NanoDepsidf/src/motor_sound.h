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

#define SNDCAL_TONE_COUNT 16 // log-spaced, 1 to 10 kHz
#define SNDCAL_LEVEL_COUNT 4 // each tone is asked from the quietest level up
#define SNDCAL_CLICK_COUNT 9 // click shapes: each haptic profile picks one (HAPTICS -> CLICK)
#define SNDCAL_NOT_HEARD 0xFF

// The click shapes. A click is one or two parts sounding together or one after the other,
// each a wave dying away. Longer rings the body up further and reads louder; a square wave has
// a quarter more fundamental than a sine of the same voltage; a chirp lets the pitch fall to
// MOTOR_SOUND_CHIRP_END of where it starts and crosses more of the body's resonances than one
// steady pitch, which rings hollow; noise is what a mechanical detent mostly is. Lengths are
// times, not cycle counts, so a low PITCH doesn't stretch a click (a stretched chirp is a bird).
#define MOTOR_SOUND_SINE 0
#define MOTOR_SOUND_SQUARE 1
#define MOTOR_SOUND_NOISE 2 // cycles of a sine at the part's pitch, each upside down or not at random
#define MOTOR_SOUND_PARTS 2
typedef struct {
    uint16_t tau_us, len_us; // the envelope's time constant, and how long it plays; len 0 = no such part
    uint8_t wave, chirp;
    uint16_t pitch_pct; // of the click's frequency
    uint8_t level_pct;  // of the click's voltage
    uint16_t delay_us;  // it starts this long after the click
} motor_sound_part_t;
typedef struct {
    motor_sound_part_t part[MOTOR_SOUND_PARTS];
} motor_sound_shape_t;
#define MS_NONE {0, 0, 0, 0, 0, 0, 0}
#define MS_ONE(tau, len, wave, chirp) {{{tau, len, wave, chirp, 100, 100, 0}, MS_NONE}}
static const motor_sound_shape_t MOTOR_SOUND_SHAPE[SNDCAL_CLICK_COUNT] = {
    MS_ONE(2000, 3000, MOTOR_SOUND_SINE, 0), MS_ONE(4000, 6000, MOTOR_SOUND_SINE, 0),
    MS_ONE(2000, 3000, MOTOR_SOUND_SQUARE, 0), MS_ONE(4000, 6000, MOTOR_SOUND_SQUARE, 0),
    MS_ONE(4000, 6000, MOTOR_SOUND_SINE, 1), MS_ONE(4000, 6000, MOTOR_SOUND_SQUARE, 1),
    // Possible since sounds are stepped at 32 kHz and reach 10 kHz (2026-10-07):
    // TICK: short, dry and an octave up.
    {{{1500, 2500, MOTOR_SOUND_SINE, 0, 200, 100, 0}, MS_NONE}},
    // TING: two pitches that aren't harmonics of each other, as a small bell has.
    {{{6000, 9000, MOTOR_SOUND_SINE, 0, 100, 100, 0}, {6000, 9000, MOTOR_SOUND_SINE, 0, 270, 70, 0}}},
    // TAP: a knock, noise only.
    {{{2000, 3000, MOTOR_SOUND_NOISE, 0, 100, 100, 0}, MS_NONE}},
};
// There were fourteen for an afternoon, and eight before that (SQR 8MS and SQR 8MS CH were
// the fifth and the eighth). Dropped after listening on hardware: SQR 8MS CH, SNAP, TOCK and
// DOUBLE, which left ten, and then SQR 8MS ("too chirpy and piezo like"). What a profile
// stored then becomes, by its old index (menu.c): fourteen to ten, ten to today's nine.
static const uint8_t MOTOR_SOUND_SHAPE_FROM_14[14] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 3, 8, 9, 9, 2};
static const uint8_t MOTOR_SOUND_SHAPE_FROM_10[10] = {0, 1, 2, 3, 3, 4, 5, 6, 7, 8};
#undef MS_ONE
#undef MS_NONE
#define MOTOR_SOUND_CHIRP_END 0.6f
#define MOTOR_SOUND_SHAPE_DEFAULT 1 // a profile's factory click
// From the click's start to the end of its last part.
static inline uint32_t motor_sound_shape_us(const motor_sound_shape_t *sh) {
    uint32_t us = 0;
    for (int i = 0; i < MOTOR_SOUND_PARTS; i++) {
        uint32_t end = sh->part[i].len_us ? (uint32_t)sh->part[i].delay_us + sh->part[i].len_us : 0;
        if (end > us) us = end;
    }
    return us;
}
// Names. The plain waves: sine or square, the time constant, CH for the chirp. The short ones
// are for the Haptics ring, where a value has six characters.
static const char *const MOTOR_SOUND_SHAPE_NAME[SNDCAL_CLICK_COUNT] = {
    "SIN 2MS", "SIN 4MS", "SQR 2MS", "SQR 4MS", "SIN 4MS CH", "SQR 4MS CH", "TICK", "TING", "TAP",
};
static const char *const MOTOR_SOUND_SHAPE_SHORT[SNDCAL_CLICK_COUNT] = {
    "SIN 2", "SIN 4", "SQR 2", "SQR 4", "SIN 4C", "SQR 4C", "TICK", "TING", "TAP",
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
// A list hit its end (control_task.c's wall): a low knock in place of the click, at the
// profile's AMP.
void motor_sound_thud(float amp);
// The startup chime, once haptics are up: three notes, C7 E7 G7, where the motor carries. On
// the click's axis.
void motor_sound_chime(void);
// The other little tunes, from any task: they play on the next tick, in place of one playing.
typedef enum {
    MOTOR_SOUND_JINGLE_CHIME = 0,
    MOTOR_SOUND_JINGLE_SAVE,   // two notes up: something was saved
    MOTOR_SOUND_JINGLE_CANCEL, // one low note: an edit was put back, or a save failed
    MOTOR_SOUND_JINGLE_AGENT,  // two soft notes: an agent is waiting for an answer
    MOTOR_SOUND_JINGLE_COUNT,
} motor_sound_jingle_t;
void motor_sound_jingle(motor_sound_jingle_t which);
// DEVICE -> CLICK: the axis (0 d, 1 q), live.
int motor_sound_axis(void);
void motor_sound_set_axis(int axis);
// This tick's sounds, given the haptic q-axis voltage: fills `out`'s voices and clip limits
// (motor_driver.h; the phase voltages and axes are the caller's to fill) and returns the
// voices' peak voltages summed, 0 when nothing sounds. Torque has priority: the sound gets
// what `vq` leaves inside the half-bus circle. Once a tick, sounding or not.
struct motor_tone;
float motor_sound_tick(float vq, struct motor_tone *out);
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
