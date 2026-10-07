#include "motor_sound.h"
#include "tasks_common.h"
#include "motor_config.h"
#include "motor_driver.h"
#include "config_store.h"
#include "menu.h"
#include "ui_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "sndcal";

#define LD(a) atomic_load_explicit(&(a), memory_order_relaxed)
#define ST(a, v) atomic_store_explicit(&(a), (v), memory_order_relaxed)

#define DT_S (CONTROL_LOOP_PERIOD_US / 1000000.0f)
#define TWO_PI 6.2831853f

// What a click may be pitched to. Sounds are stepped at the PWM rate, 32 kHz
// (motor_driver_tone()), so the top is set by what is heard, not by the loop: on hardware
// (2026-10-07) tones up to 10 kHz were about as loud as the 3-4 kHz ones.
#define SOUND_F_MIN_HZ 500.0f
#define SOUND_F_MAX_HZ 10000.0f
// SOUND CAL's tones. Under 1 kHz the motor is nearly silent, so they start there.
#define CAL_F_MIN_HZ 1000.0f
#define CAL_F_MAX_HZ 10000.0f

// The voltage a sound may use. Torque comes first: Vq keeps its own cap
// (MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) and Vd gets what is left inside the half-bus
// circle, the most the sine PWM can put out. A burst lasts a few milliseconds and at these
// frequencies the windings' inductance holds the current well under the DC figure, so the
// current-derived cap is not applied to it. The limit follows the chord of the circle from
// (Vq 0, Vd max) to (Vq cap, Vd left there): never above the circle, and no square root in
// the loop. The sound is clipped to it, sample by sample, where it is played.
#define SOUND_VMAX_V MOTOR_HALF_BUS_LIMIT_V
#define SOUND_VQ_CAP_V MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V
static const float SOUND_CHORD_K =
    (SOUND_VMAX_V - __builtin_sqrtf(SOUND_VMAX_V * SOUND_VMAX_V - SOUND_VQ_CAP_V * SOUND_VQ_CAP_V)) / SOUND_VQ_CAP_V;

// Until SOUND CAL has run. First result on hardware (2026-10-07): the sound is faint, loudest
// at 3-4 kHz, so the default sits there and is long for a click.
#define CLICK_DEFAULT_HZ 3500.0f

#define SHAPE MOTOR_SOUND_SHAPE
#define CHIRP_END MOTOR_SOUND_CHIRP_END
// Per shape, worked out once at start-up (motor_sound_init(), before the control loop runs).
static struct {
    uint32_t ticks, delay; // 0 ticks = no such part
    float decay;           // envelope factor per tick
    float glide;           // pitch factor per tick: under 1 is the chirp
    float pitch, level;
    uint8_t wave;
} s_part[SNDCAL_CLICK_COUNT + 1][MOTOR_SOUND_PARTS]; // and the end-stop knock, SHAPE_THUD
_Static_assert(MOTOR_SOUND_SINE == MOTOR_TONE_SINE && MOTOR_SOUND_SQUARE == MOTOR_TONE_SQUARE && MOTOR_SOUND_NOISE == MOTOR_TONE_NOISE,
               "a part's wave goes to the driver as it is");

// The little tunes: a list of notes, each with its start, length, pitch, wave, level and decay
// (per second); `slide_pct` is the pitch it glides to by its end (100 = steady). Note n plays
// on voice n mod 3, taking it from the note three before. All high: at 500 to 800 Hz the motor
// is nearly silent (the old speaker's chime was C5 E5 G5). Notes that overlap on the q axis
// share the voltage. Not const: in RAM, with the rest the loop reads. A MOTOR_TONE_RICH note's
// fundamental is two thirds of its peak, a square's 1.27 of it.
#define TUNE_VOICES 3
#define TUNE_NOTES 16
#define SI MOTOR_TONE_SINE
#define SQ MOTOR_TONE_SQUARE
#define RI MOTOR_TONE_RICH
typedef struct {
    uint16_t at_ms, len_ms;
    float hz;
    uint8_t wave, level_pct, slide_pct;
    float decay;
} tune_note_t;
static struct {
    uint8_t notes;
    float volts;
    tune_note_t note[TUNE_NOTES];
    // worked out at start-up
    uint32_t at[TUNE_NOTES], len[TUNE_NOTES];
    float decay_tick[TUNE_NOTES], glide[TUNE_NOTES];
} s_tune[MOTOR_SOUND_JINGLE_COUNT] = {
    // CHIME: C7 E7 G7, ringing over each other. The startup chime until 2026-10-07; now an
    // agent asking for approval.
    [MOTOR_SOUND_JINGLE_CHIME] = {3, SOUND_VMAX_V, {{0, 280, 2093.0f, RI, 100, 100, 25.0f}, {80, 280, 2637.0f, RI, 100, 100, 25.0f},
                                                    {160, 280, 3136.0f, RI, 100, 100, 25.0f}}},
    [MOTOR_SOUND_JINGLE_SAVE] = {2, 1.8f, {{0, 200, 3136.0f, RI, 100, 100, 35.0f}, {70, 200, 4186.0f, RI, 100, 100, 35.0f}}},
    [MOTOR_SOUND_JINGLE_CANCEL] = {1, 2.2f, {{0, 233, 1568.0f, RI, 100, 100, 30.0f}}},
    [MOTOR_SOUND_JINGLE_AGENT] = {2, 1.6f, {{0, 583, 2637.0f, SI, 100, 100, 12.0f}, {120, 583, 3520.0f, SI, 100, 100, 12.0f}}},
    // COIN, the startup chime since 2026-10-07: B5, then E6 held; the pair again, quieter, as
    // an echo. Picked by ear from six in the manner of game sounds. Squares an octave up at
    // 45 to 75 ms a note were "very high" and "overdriven"; of the plucked rich-wave ones in C6
    // to C7 the others were too quiet on the d axis, and some cut the USB power on the q axis,
    // as TOCK did (burst_arm()). Keep a new tune near these two, or try it on both axes.
    [MOTOR_SOUND_JINGLE_COIN] = {4, SOUND_VMAX_V, {
        {0, 90, 987.8f, RI, 100, 100, 9.0f}, {100, 450, 1318.5f, RI, 100, 100, 6.0f}, {650, 90, 987.8f, RI, 35, 100, 9.0f},
        {750, 380, 1318.5f, RI, 35, 100, 6.0f},
    }},
    // ALLOW: COIN's two notes with no echo.
    [MOTOR_SOUND_JINGLE_ALLOW] = {2, SOUND_VMAX_V, {{0, 90, 987.8f, RI, 100, 100, 9.0f}, {100, 400, 1318.5f, RI, 100, 100, 7.0f}}},
};
#undef SI
#undef SQ
#undef RI
// A note fades out over its last 2 ms, or a held one would end with a click.
#define TUNE_FADE_TICKS 20
#define TUNE_FADE_K 0.8f

// The end-stop knock (motor_sound_thud()): a click shape of its own, low and noisy.
// On the d axis only (burst_arm()), where parts may stack: both at full level, and long, as
// the d axis is the quieter one.
static const motor_sound_shape_t THUD = {{{6000, 9000, MOTOR_SOUND_SQUARE, 0, 50, 100, 0}, {3000, 4500, MOTOR_SOUND_NOISE, 0, 100, 100, 0}}};
#define SHAPE_THUD SNDCAL_CLICK_COUNT

// --- SOUND CAL's programme ---
#define CAL_TONE_MS 300
#define CAL_GAP_MS 150 // silence after every sound: with the tone length, the heat limit (below)
#define CAL_SETTLE_MS 300
#define CAL_SWEEP_V 1.4f
static const float CAL_LEVEL_V[SNDCAL_LEVEL_COUNT] = {0.3f, 0.7f, 1.4f, 2.5f};
// A tone fades in and out over 5 ms, or its edges would click.
#define TONE_SLEW_V_PER_TICK (SOUND_VMAX_V / (0.005f / DT_S))
// The knob turning this far while a sound plays stops the calibration: a hand is on it, or
// the d axis is off and the sound is leaking into torque. A q-axis sound is torque: with no
// spring to hold it the knob creeps a little by itself, so it is allowed more.
#define CAL_MOVED_RAD 0.05f
#define CAL_MOVED_Q_RAD 0.35f

// --- Shared with the control loop (Core 1 writes, Core 0 reads) ---
// The click in use: SOUND CAL finds the frequency, it or DEVICE -> CLICK picks the axis. The
// wave comes with each click, from the haptic profile.
static _Atomic float s_click_hz = CLICK_DEFAULT_HZ;
static _Atomic bool s_click_q = false; // on the q axis, not the d axis
// SOUND CAL
static _Atomic bool s_cal_on = false;
static _Atomic bool s_cal_request = false;
static _Atomic bool s_cal_moved = false;
static _Atomic uint8_t s_cal_keys = 0;
static _Atomic float s_tone_hz = 0.0f;
static _Atomic float s_tone_v = 0.0f; // 0 = no tone
static _Atomic bool s_tone_q = false;
static sndcal_view_t s_view;
static uint8_t s_heard[SNDCAL_TONE_COUNT]; // the last calibration's answers, kept for the next save
static bool s_calibrated = false;
static uint8_t s_legacy_shape = MOTOR_SOUND_SHAPE_DEFAULT;
static portMUX_TYPE s_view_mux = portMUX_INITIALIZER_UNLOCKED;

// --- Core 0 ---

// The voices handed to the motor driver: the click's parts (or SOUND CAL's tone, which never
// plays with one) and the chime's three notes.
#define VOICE_CLICK 0
#define VOICE_JINGLE MOTOR_SOUND_PARTS
_Static_assert(MOTOR_TONE_VOICES >= VOICE_JINGLE + TUNE_VOICES, "a voice for each part of a click and for each note");

static struct {
    struct {
        uint32_t wait, left; // ticks until it starts, and still to play
        float hz, env, decay, glide;
        uint8_t wave;
        bool fresh, q;
    } part[MOTOR_SOUND_PARTS];
} s_burst;
static float s_tone_amp = 0.0f;

static _Atomic int s_jingle_request = -1; // a motor_sound_jingle_t to start, from any task
static struct {
    int which; // -1 = not playing
    uint32_t tick;
    int next; // the next note to start
    bool q;
    struct {
        uint32_t left; // ticks still to play
        float hz, env, decay, glide;
        uint8_t wave;
        bool fresh;
    } voice[TUNE_VOICES];
} s_jingle = {.which = -1};

void CONTROL_HOT motor_sound_jingle(motor_sound_jingle_t which) {
    ST(s_jingle_request, (int)which);
}

void CONTROL_HOT motor_sound_chime(void) {
    motor_sound_jingle(MOTOR_SOUND_JINGLE_COIN);
}

static float CONTROL_HOT clamp_hz(float hz) {
    return hz < SOUND_F_MIN_HZ ? SOUND_F_MIN_HZ : hz > SOUND_F_MAX_HZ ? SOUND_F_MAX_HZ : hz;
}

// The end-stop knock stays off the q axis. TOCK, a click built the same way (a low square
// under noise), cut the USB power on the q axis on hardware, 2026-10-07 (reset reason
// POWERON): first with one click, then, once q-axis voices shared the voltage, only with the
// SINE feel. On the d axis it was fine. The cause was not found and TOCK was dropped; the
// knock plays while the knob is pushed into a wall, where a power cut would be worst.
static void CONTROL_HOT burst_arm(float hz, int shape, float volts, bool q) {
    if (shape == SHAPE_THUD) q = false;
    for (int i = 0; i < MOTOR_SOUND_PARTS; i++) {
        s_burst.part[i].q = q;
        s_burst.part[i].wave = s_part[shape][i].wave;
        s_burst.part[i].glide = s_part[shape][i].glide;
        s_burst.part[i].decay = s_part[shape][i].decay;
        s_burst.part[i].wait = s_part[shape][i].delay;
        s_burst.part[i].left = s_part[shape][i].ticks;
        s_burst.part[i].hz = clamp_hz(hz * s_part[shape][i].pitch);
        s_burst.part[i].env = volts * s_part[shape][i].level;
        s_burst.part[i].fresh = true;
    }
}

void CONTROL_HOT motor_sound_click(float pitch, float amp, int shape) {
    if (amp <= 0.0f || shape < 0 || shape >= SNDCAL_CLICK_COUNT) return;
    float hz = clamp_hz(LD(s_click_hz) * pitch);
    // AMP on a curve, a(2 - a): the motor is faint, so the low settings get more of the
    // voltage than a straight line gives (15% -> 28%, 50% -> 75%).
    burst_arm(hz, shape, SOUND_VMAX_V * amp * (2.0f - amp), LD(s_click_q));
}

void CONTROL_HOT motor_sound_thud(float amp) {
    if (amp <= 0.0f) return;
    burst_arm(LD(s_click_hz), SHAPE_THUD, SOUND_VMAX_V * amp * (2.0f - amp), LD(s_click_q));
}

int CONTROL_HOT motor_sound_axis(void) { return LD(s_click_q); }
void CONTROL_HOT motor_sound_set_axis(int axis) { ST(s_click_q, axis != 0); }

float CONTROL_HOT motor_sound_tick(float vq, struct motor_tone *out) {
    motor_tone_voice_t *voice = out->voice;
    for (int i = 0; i < MOTOR_TONE_VOICES; i++) voice[i].volts = 0.0f;
    float total = 0.0f, on_q = 0.0f; // the peaks, summed: all of them, and those on the q axis
    if (LD(s_cal_on)) {
        bool was_silent = s_tone_amp == 0.0f;
        float target = LD(s_tone_v);
        if (s_tone_amp < target) {
            s_tone_amp += TONE_SLEW_V_PER_TICK;
            if (s_tone_amp > target) s_tone_amp = target;
        } else if (s_tone_amp > target) {
            s_tone_amp -= TONE_SLEW_V_PER_TICK;
            if (s_tone_amp < target) s_tone_amp = target;
        }
        if (s_tone_amp > 0.0f) {
            // A q-axis tone starts at its peak: from zero, a sine's torque would set the free
            // knob drifting one way for as long as the tone lasts.
            bool q = LD(s_tone_q);
            voice[VOICE_CLICK] = (motor_tone_voice_t){.hz = LD(s_tone_hz), .volts = s_tone_amp, .q = q, .start = was_silent, .cosine = q};
            total += s_tone_amp;
            if (q) on_q += s_tone_amp;
        }
    } else {
        s_tone_amp = 0.0f;
        for (int i = 0; i < MOTOR_SOUND_PARTS; i++) {
            if (s_burst.part[i].left == 0) continue;
            if (s_burst.part[i].wait > 0) {
                s_burst.part[i].wait--;
                continue;
            }
            voice[VOICE_CLICK + i] = (motor_tone_voice_t){.hz = s_burst.part[i].hz, .volts = s_burst.part[i].env, .q = s_burst.part[i].q,
                                                          .wave = s_burst.part[i].wave, .start = s_burst.part[i].fresh};
            total += s_burst.part[i].env;
            if (s_burst.part[i].q) on_q += s_burst.part[i].env;
            s_burst.part[i].fresh = false;
            s_burst.part[i].hz *= s_burst.part[i].glide;
            s_burst.part[i].env *= s_burst.part[i].decay;
            s_burst.part[i].left--;
        }
    }
    int request = atomic_exchange_explicit(&s_jingle_request, -1, memory_order_relaxed);
    if (request >= 0 && request < MOTOR_SOUND_JINGLE_COUNT) {
        s_jingle.which = request;
        s_jingle.tick = 0;
        s_jingle.next = 0;
        s_jingle.q = LD(s_click_q);
        for (int i = 0; i < TUNE_VOICES; i++) s_jingle.voice[i].left = 0;
    }
    if (s_jingle.which >= 0) {
        int w = s_jingle.which;
        while (s_jingle.next < s_tune[w].notes && s_jingle.tick >= s_tune[w].at[s_jingle.next]) {
            int n = s_jingle.next++, v = n % TUNE_VOICES;
            s_jingle.voice[v].left = s_tune[w].len[n];
            s_jingle.voice[v].hz = s_tune[w].note[n].hz;
            s_jingle.voice[v].env = s_tune[w].volts * s_tune[w].note[n].level_pct * 0.01f;
            s_jingle.voice[v].decay = s_tune[w].decay_tick[n];
            s_jingle.voice[v].glide = s_tune[w].glide[n];
            s_jingle.voice[v].wave = s_tune[w].note[n].wave;
            s_jingle.voice[v].fresh = true;
        }
        bool playing = s_jingle.next < s_tune[w].notes;
        for (int i = 0; i < TUNE_VOICES; i++) {
            if (s_jingle.voice[i].left == 0) continue;
            playing = true;
            voice[VOICE_JINGLE + i] = (motor_tone_voice_t){.hz = s_jingle.voice[i].hz, .volts = s_jingle.voice[i].env, .q = s_jingle.q,
                                                           .wave = s_jingle.voice[i].wave, .start = s_jingle.voice[i].fresh};
            total += s_jingle.voice[i].env;
            if (s_jingle.q) on_q += s_jingle.voice[i].env;
            s_jingle.voice[i].fresh = false;
            s_jingle.voice[i].hz *= s_jingle.voice[i].glide;
            s_jingle.voice[i].env *= --s_jingle.voice[i].left < TUNE_FADE_TICKS ? TUNE_FADE_K : s_jingle.voice[i].decay;
        }
        s_jingle.tick++;
        if (!playing) s_jingle.which = -1;
    }
    // Voices on the q axis share the voltage: together they are never more than one voice at
    // full level. Stacked past that, the sum is clipped, and clipping two pitches that are
    // close makes their difference tone, a slow one: on the q axis that is torque. TOCK (a
    // square at 0.75 of the pitch under noise at the pitch, both at full level) cut the USB
    // power with one click on the q axis, while its square alone, and TOCK on the d axis,
    // were fine (2026-10-07). On the d axis stacking stays: it is louder and makes no torque.
    if (on_q > SOUND_VMAX_V) {
        float k = SOUND_VMAX_V / on_q;
        for (int i = 0; i < MOTOR_TONE_VOICES; i++) {
            if (voice[i].volts > 0.0f && voice[i].q) voice[i].volts *= k;
        }
        total -= on_q - SOUND_VMAX_V;
        on_q = SOUND_VMAX_V;
    }
    // On the q axis the sound adds to the torque voltage, up to the half bus.
    out->q_min = -SOUND_VMAX_V - vq;
    out->q_max = SOUND_VMAX_V - vq;
    // On the d axis it gets what q leaves: the chord, which only holds up to Vq's own cap.
    float qa = (vq < 0.0f ? -vq : vq) + on_q;
    out->d_lim = qa > SOUND_VQ_CAP_V ? 0.0f : SOUND_VMAX_V - qa * SOUND_CHORD_K;
    return total;
}

bool CONTROL_HOT motor_sound_cal_active(void) {
    return LD(s_cal_on);
}

void CONTROL_HOT motor_sound_cal_keys(uint8_t pressed) {
    if (pressed) atomic_fetch_or_explicit(&s_cal_keys, pressed, memory_order_relaxed);
}

void CONTROL_HOT motor_sound_cal_angle(float mech_rad) {
    // Measured from where the knob was when the sound started; between sounds it may move.
    static bool s_sounding = false;
    static float s_ref = 0.0f;
    bool sounding = s_tone_amp > 0.0f;
    float limit = LD(s_tone_q) ? CAL_MOVED_Q_RAD : CAL_MOVED_RAD;
    if (sounding && !s_sounding) s_ref = mech_rad;
    s_sounding = sounding;
    if (!sounding) return;
    float d = mech_rad - s_ref;
    if (d > (float)M_PI) d -= TWO_PI;
    if (d < -(float)M_PI) d += TWO_PI;
    if (d > limit || d < -limit) ST(s_cal_moved, true);
}

void CONTROL_HOT motor_sound_cal_start(void) {
    ST(s_cal_request, true);
}

// --- Core 1: the calibration ---

static uint16_t tone_hz(int i) {
    return (uint16_t)lroundf(CAL_F_MIN_HZ * powf(CAL_F_MAX_HZ / CAL_F_MIN_HZ, (float)i / (SNDCAL_TONE_COUNT - 1)));
}

static void apply_click(float hz, bool q, bool calibrated) {
    ST(s_click_hz, hz);
    ST(s_click_q, q);
    s_calibrated = calibrated;
    portENTER_CRITICAL(&s_view_mux);
    s_view.calibrated = calibrated;
    s_view.click_hz = (uint16_t)lroundf(hz);
    portEXIT_CRITICAL(&s_view_mux);
}

static void view_step(sndcal_stage_t stage, int step, int level, uint16_t hz, bool q) {
    portENTER_CRITICAL(&s_view_mux);
    s_view.stage = stage;
    s_view.axis = q;
    s_view.step = step;
    s_view.level = level;
    s_view.freq_hz = hz;
    portEXIT_CRITICAL(&s_view_mux);
}

static void view_playing(bool on) {
    portENTER_CRITICAL(&s_view_mux);
    s_view.playing = on;
    portEXIT_CRITICAL(&s_view_mux);
}

static void view_stage(sndcal_stage_t stage) {
    portENTER_CRITICAL(&s_view_mux);
    s_view.stage = stage;
    s_view.playing = false;
    portEXIT_CRITICAL(&s_view_mux);
}

// False once the calibration has to stop: the screen was left, or the knob turned.
static bool cal_ok(void) {
    return menu_current_screen() == MENU_SCREEN_SOUND_CAL && !LD(s_cal_moved);
}

static bool cal_wait(int ms) {
    for (int t = 0; t < ms; t += 10) {
        if (!cal_ok()) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return cal_ok();
}

// Waits for F1, F2 or F3; 0 when the calibration has to stop.
static uint8_t cal_key(void) {
    atomic_exchange_explicit(&s_cal_keys, 0, memory_order_relaxed);
    for (;;) {
        if (!cal_ok()) return 0;
        uint8_t k = atomic_exchange_explicit(&s_cal_keys, 0, memory_order_relaxed);
        if (k & UI_BTN_F1) return UI_BTN_F1;
        if (k & UI_BTN_F3) return UI_BTN_F3;
        if (k & UI_BTN_F2) return UI_BTN_F2;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Every sound is followed by CAL_GAP_MS of silence, so the windings never see more than
// CAL_TONE_MS in CAL_TONE_MS + CAL_GAP_MS of a tone: under what a held detent may draw.
static bool play_tone(uint16_t hz, float volts, bool q) {
    ST(s_tone_q, q);
    ST(s_tone_hz, (float)hz);
    ST(s_tone_v, volts);
    view_playing(true);
    bool ok = cal_wait(CAL_TONE_MS);
    ST(s_tone_v, 0.0f);
    view_playing(false);
    return ok && cal_wait(CAL_GAP_MS);
}

// The best tone is the one heard at the quietest level; of several, the middle one.
static int pick_best(const uint8_t *heard) {
    uint8_t low = SNDCAL_NOT_HEARD;
    for (int i = 0; i < SNDCAL_TONE_COUNT; i++) {
        if (heard[i] >= 1 && heard[i] < low) low = heard[i];
    }
    if (low == SNDCAL_NOT_HEARD) return -1;
    int tied[SNDCAL_TONE_COUNT], n = 0;
    for (int i = 0; i < SNDCAL_TONE_COUNT; i++) {
        if (heard[i] == low) tied[n++] = i;
    }
    return tied[n / 2];
}

// Returns the stage to leave on screen.
static sndcal_stage_t run_cal(void) {
    uint8_t heard[SNDCAL_TONE_COUNT] = {0};
    portENTER_CRITICAL(&s_view_mux);
    memset(s_view.heard, 0, sizeof(s_view.heard));
    s_view.best = SNDCAL_NOT_HEARD;
    portEXIT_CRITICAL(&s_view_mux);
    ESP_LOGI(TAG, "sndcal start");
    if (!cal_wait(CAL_SETTLE_MS)) return SNDCAL_IDLE; // the spring is off now: let the knob come to rest

    // 1. Every tone once on the d axis, then on the q axis: which carries better, and where
    // the knob rings.
    bool q = false;
    for (;;) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < SNDCAL_TONE_COUNT; i++) {
                view_step(SNDCAL_SWEEP, i, 0, tone_hz(i), pass);
                if (!play_tone(tone_hz(i), CAL_SWEEP_V, pass)) return SNDCAL_IDLE;
            }
            if (!cal_wait(CAL_TONE_MS)) return SNDCAL_IDLE; // a pause between the passes
        }
        view_stage(SNDCAL_AXIS_ASK);
        uint8_t key = cal_key();
        if (key == 0) return SNDCAL_IDLE;
        if (key != UI_BTN_F2) {
            q = key == UI_BTN_F3;
            break;
        }
    }
    ESP_LOGI(TAG, "sndcal axis=%c", q ? 'q' : 'd');

    // 2. Each tone from quiet to loud, until it is heard.
    for (int i = 0; i < SNDCAL_TONE_COUNT; i++) {
        uint8_t result = SNDCAL_NOT_HEARD;
        for (int level = 0; level < SNDCAL_LEVEL_COUNT; level++) {
            uint8_t key;
            do {
                view_step(SNDCAL_TONES, i, level, tone_hz(i), q);
                if (!play_tone(tone_hz(i), CAL_LEVEL_V[level], q)) return SNDCAL_IDLE;
                key = cal_key();
                if (key == 0) return SNDCAL_IDLE;
            } while (key == UI_BTN_F2);
            ESP_LOGI(TAG, "sndcal axis=%c f=%u amp=%.2f heard=%d", q ? 'q' : 'd', tone_hz(i), CAL_LEVEL_V[level],
                     key == UI_BTN_F1);
            if (key == UI_BTN_F1) {
                result = level + 1;
                break;
            }
        }
        heard[i] = result;
        portENTER_CRITICAL(&s_view_mux);
        s_view.heard[i] = result;
        portEXIT_CRITICAL(&s_view_mux);
    }
    int best = pick_best(heard);
    if (best < 0) {
        ESP_LOGI(TAG, "sndcal result: no tone heard, nothing stored");
        return SNDCAL_DONE;
    }
    portENTER_CRITICAL(&s_view_mux);
    s_view.best = best;
    portEXIT_CRITICAL(&s_view_mux);

    memcpy(s_heard, heard, sizeof(s_heard));
    apply_click(tone_hz(best), q, true);
    ESP_LOGI(TAG, "sndcal result axis=%c f=%u level=%u", q ? 'q' : 'd', tone_hz(best), heard[best]);
    motor_sound_save();
    menu_click_saved();
    return SNDCAL_DONE;
}

static void cal_task_fn(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        bool on_screen = menu_current_screen() == MENU_SCREEN_SOUND_CAL;
        if (!atomic_exchange_explicit(&s_cal_request, false, memory_order_relaxed) || !on_screen) {
            if (!on_screen) view_stage(SNDCAL_IDLE); // a result or a stop is shown until the screen is left
            continue;
        }
        // The console is quiet after boot (sysmon.c); this tag's lines are the test's record.
        esp_log_level_set(TAG, ESP_LOG_INFO);
        ST(s_cal_moved, false);
        ST(s_tone_v, 0.0f);
        ST(s_cal_on, true);
        sndcal_stage_t end = run_cal();
        ST(s_tone_v, 0.0f);
        vTaskDelay(pdMS_TO_TICKS(10)); // the tone's fade-out
        ST(s_cal_on, false);
        if (LD(s_cal_moved)) {
            ESP_LOGI(TAG, "sndcal stopped: the knob moved");
            end = SNDCAL_MOVED;
        }
        view_stage(end);
    }
}

void motor_sound_cal_view(sndcal_view_t *out) {
    portENTER_CRITICAL(&s_view_mux);
    *out = s_view;
    portEXIT_CRITICAL(&s_view_mux);
    out->click_axis = LD(s_click_q);
}

int motor_sound_legacy_shape(void) {
    return s_legacy_shape;
}

void motor_sound_save(void) {
    snd_cal_cfg_t cfg = {
        .version = SND_CAL_CFG_VERSION,
        .freq_hz = (uint16_t)lroundf(LD(s_click_hz)),
        .shape = s_legacy_shape,
        .axis = LD(s_click_q),
        .calibrated = s_calibrated,
    };
    memcpy(cfg.heard, s_heard, sizeof(cfg.heard));
    config_store_save_snd_cal(&cfg);
}

void motor_sound_init(void) {
    for (int i = 0; i <= SNDCAL_CLICK_COUNT; i++) {
        for (int k = 0; k < MOTOR_SOUND_PARTS; k++) {
            const motor_sound_part_t *pt = i == SHAPE_THUD ? &THUD.part[k] : &SHAPE[i].part[k];
            if (pt->len_us == 0) continue;
            s_part[i][k].ticks = (uint32_t)(pt->len_us / CONTROL_LOOP_PERIOD_US);
            s_part[i][k].delay = (uint32_t)(pt->delay_us / CONTROL_LOOP_PERIOD_US);
            s_part[i][k].wave = pt->wave;
            s_part[i][k].decay = expf(-DT_S / (pt->tau_us / 1000000.0f));
            s_part[i][k].glide = pt->chirp ? powf(CHIRP_END, 1.0f / s_part[i][k].ticks) : 1.0f;
            s_part[i][k].pitch = pt->pitch_pct / 100.0f;
            s_part[i][k].level = pt->level_pct / 100.0f;
        }
    }
    for (int i = 0; i < MOTOR_SOUND_JINGLE_COUNT; i++) {
        for (int n = 0; n < s_tune[i].notes; n++) {
            const tune_note_t *nt = &s_tune[i].note[n];
            s_tune[i].at[n] = (uint32_t)(nt->at_ms / 1000.0f / DT_S);
            s_tune[i].len[n] = (uint32_t)(nt->len_ms / 1000.0f / DT_S);
            s_tune[i].decay_tick[n] = expf(-nt->decay * DT_S);
            s_tune[i].glide[n] = powf(nt->slide_pct / 100.0f, 1.0f / s_tune[i].len[n]);
        }
    }
    s_view.best = SNDCAL_NOT_HEARD;
    snd_cal_cfg_t cfg;
    if (config_store_load_snd_cal(&cfg)) {
        apply_click(cfg.freq_hz, cfg.axis, cfg.calibrated);
        s_legacy_shape = MOTOR_SOUND_SHAPE_FROM_10[MOTOR_SOUND_SHAPE_FROM_14[cfg.shape < 14 ? cfg.shape : MOTOR_SOUND_SHAPE_DEFAULT]];
        memcpy(s_heard, cfg.heard, sizeof(s_heard));
        memcpy(s_view.heard, cfg.heard, sizeof(s_view.heard));
    } else {
        apply_click(CLICK_DEFAULT_HZ, false, false);
    }
    // Internal stack: the task writes NVS.
    xTaskCreatePinnedToCore(cal_task_fn, "sndcal", 4096, NULL, PRIO_STORE, NULL, CORE_IO);
}
