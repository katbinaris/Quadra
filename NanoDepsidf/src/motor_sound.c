#include "motor_sound.h"
#include "tasks_common.h"
#include "motor_config.h"
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

// Tones the 10 kHz loop can play: by 5 kHz (two samples a cycle) the amplitude depends on
// where the samples happen to fall, so everything stops at 4 kHz.
#define SOUND_F_MIN_HZ 500.0f
#define SOUND_F_MAX_HZ 4000.0f

// The voltage a sound may use. Torque comes first: Vq keeps its own cap
// (MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) and Vd gets what is left inside the half-bus
// circle, the most the sine PWM can put out. A burst lasts a few milliseconds and at these
// frequencies the windings' inductance holds the current well under the DC figure, so the
// current-derived cap is not applied to it. The limit follows the chord of the circle from
// (Vq 0, Vd max) to (Vq cap, Vd left there): never above the circle, and no square root in
// the loop.
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
static float s_shape_decay[SNDCAL_CLICK_COUNT]; // envelope factor per tick
static float s_shape_glide[SNDCAL_CLICK_COUNT]; // pitch factor per tick: under 1 is the chirp
static uint32_t s_shape_ticks[SNDCAL_CLICK_COUNT];
static bool s_shape_square[SNDCAL_CLICK_COUNT];

// The startup chime: an arpeggio, C7 E7 G7, 80 ms apart, decaying at 25/s. (The speaker's was
// C5 E5 G5; at 500 to 800 Hz the motor is nearly silent, and these sit just under where it is
// loudest.) Three voices can overlap, so each gets a share of the voltage.
#define CHIME_NOTES 3
#define CHIME_HZ {2093.0f, 2637.0f, 3136.0f}
#define CHIME_STEP_TICKS ((uint32_t)(0.08f / DT_S))
#define CHIME_TICKS (2 * CHIME_STEP_TICKS + (uint32_t)(7.0f / 25.0f / DT_S)) // to about -60 dB after the last note
#define CHIME_NOTE_V (SOUND_VMAX_V / 1.5f)
static const float CHIME_DECAY = __builtin_expf(-25.0f * DT_S); // envelope factor per tick

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

static struct {
    uint32_t left; // ticks still to play
    float phase, inc, env, decay, glide;
    bool q, square;
} s_burst;
static float s_tone_phase = 0.0f;
static float s_tone_amp = 0.0f;

static struct {
    uint32_t tick; // CHIME_TICKS = not playing
    float phase[CHIME_NOTES], inc[CHIME_NOTES], env[CHIME_NOTES];
    bool q;
} s_chime = {.tick = CHIME_TICKS};

void CONTROL_HOT motor_sound_chime(void) {
    static float hz[CHIME_NOTES] = CHIME_HZ; // not const: in RAM, with the rest the loop reads
    for (int i = 0; i < CHIME_NOTES; i++) {
        s_chime.phase[i] = 0.0f;
        s_chime.inc[i] = TWO_PI * hz[i] * DT_S;
        s_chime.env[i] = CHIME_NOTE_V;
    }
    s_chime.q = LD(s_click_q);
    s_chime.tick = 0;
}

static void CONTROL_HOT burst_arm(float hz, int shape, float volts, bool q) {
    s_burst.q = q;
    s_burst.square = s_shape_square[shape];
    s_burst.glide = s_shape_glide[shape];
    s_burst.decay = s_shape_decay[shape];
    s_burst.left = s_shape_ticks[shape];
    s_burst.phase = 0.0f;
    s_burst.inc = TWO_PI * hz * DT_S;
    s_burst.env = volts;
}

void CONTROL_HOT motor_sound_click(float pitch, float amp, int shape) {
    if (amp <= 0.0f || shape < 0 || shape >= SNDCAL_CLICK_COUNT) return;
    float hz = LD(s_click_hz) * pitch;
    if (hz < SOUND_F_MIN_HZ) hz = SOUND_F_MIN_HZ;
    if (hz > SOUND_F_MAX_HZ) hz = SOUND_F_MAX_HZ;
    // AMP on a curve, a(2 - a): the motor is faint, so the low settings get more of the
    // voltage than a straight line gives (15% -> 28%, 50% -> 75%).
    burst_arm(hz, shape, SOUND_VMAX_V * amp * (2.0f - amp), LD(s_click_q));
}

int CONTROL_HOT motor_sound_axis(void) { return LD(s_click_q); }
void CONTROL_HOT motor_sound_set_axis(int axis) { ST(s_click_q, axis != 0); }

void CONTROL_HOT motor_sound_dq(float vq, float *vd_out, float *vq_out) {
    float sd = 0.0f, sq = 0.0f; // the sound, per axis
    if (LD(s_cal_on)) {
        float target = LD(s_tone_v);
        if (s_tone_amp < target) {
            s_tone_amp += TONE_SLEW_V_PER_TICK;
            if (s_tone_amp > target) s_tone_amp = target;
        } else if (s_tone_amp > target) {
            s_tone_amp -= TONE_SLEW_V_PER_TICK;
            if (s_tone_amp < target) s_tone_amp = target;
        }
        // A q-axis tone starts at its peak: from zero, a sine's torque would set the free knob
        // drifting one way for as long as the tone lasts.
        if (s_tone_amp == 0.0f) s_tone_phase = LD(s_tone_q) ? TWO_PI / 4.0f : 0.0f;
        if (s_tone_amp > 0.0f) {
            s_tone_phase += TWO_PI * LD(s_tone_hz) * DT_S;
            if (s_tone_phase >= TWO_PI) s_tone_phase -= TWO_PI;
            float v = s_tone_amp * sinf(s_tone_phase);
            if (LD(s_tone_q)) sq += v;
            else sd += v;
        }
    } else {
        s_tone_amp = 0.0f;
        s_tone_phase = 0.0f;
    }
    if (s_chime.tick < CHIME_TICKS) {
        float v = 0.0f;
        for (int i = 0; i < CHIME_NOTES; i++) {
            if (s_chime.tick < i * CHIME_STEP_TICKS) break; // this note hasn't started
            v += s_chime.env[i] * sinf(s_chime.phase[i]);
            s_chime.phase[i] += s_chime.inc[i];
            if (s_chime.phase[i] >= TWO_PI) s_chime.phase[i] -= TWO_PI;
            s_chime.env[i] *= CHIME_DECAY;
        }
        if (s_chime.q) sq += v;
        else sd += v;
        s_chime.tick++;
    }
    if (s_burst.left > 0) {
        float w = sinf(s_burst.phase);
        if (s_burst.square) w = w >= 0.0f ? 1.0f : -1.0f;
        if (s_burst.q) sq += s_burst.env * w;
        else sd += s_burst.env * w;
        s_burst.phase += s_burst.inc;
        if (s_burst.phase >= TWO_PI) s_burst.phase -= TWO_PI;
        s_burst.inc *= s_burst.glide;
        s_burst.env *= s_burst.decay;
        s_burst.left--;
    }
    // On the q axis the sound adds to the torque voltage, up to the half bus.
    float q = vq + sq;
    if (q > SOUND_VMAX_V) q = SOUND_VMAX_V;
    if (q < -SOUND_VMAX_V) q = -SOUND_VMAX_V;
    // On the d axis it gets what q leaves: the chord, which only holds up to Vq's own cap.
    float qa = q < 0.0f ? -q : q;
    float lim = qa > SOUND_VQ_CAP_V ? 0.0f : SOUND_VMAX_V - qa * SOUND_CHORD_K;
    if (sd > lim) sd = lim;
    if (sd < -lim) sd = -lim;
    *vd_out = sd;
    *vq_out = q;
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
    return (uint16_t)lroundf(SOUND_F_MIN_HZ * powf(SOUND_F_MAX_HZ / SOUND_F_MIN_HZ, (float)i / (SNDCAL_TONE_COUNT - 1)));
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
    for (int i = 0; i < SNDCAL_CLICK_COUNT; i++) {
        s_shape_ticks[i] = (uint32_t)(SHAPE[i].len_us / CONTROL_LOOP_PERIOD_US);
        s_shape_square[i] = SHAPE[i].square;
        s_shape_decay[i] = expf(-DT_S / (SHAPE[i].tau_us / 1000000.0f));
        s_shape_glide[i] = SHAPE[i].chirp ? powf(CHIRP_END, 1.0f / s_shape_ticks[i]) : 1.0f;
    }
    s_view.best = SNDCAL_NOT_HEARD;
    snd_cal_cfg_t cfg;
    if (config_store_load_snd_cal(&cfg)) {
        apply_click(cfg.freq_hz, cfg.axis, cfg.calibrated);
        s_legacy_shape = cfg.shape;
        memcpy(s_heard, cfg.heard, sizeof(s_heard));
        memcpy(s_view.heard, cfg.heard, sizeof(s_view.heard));
    } else {
        apply_click(CLICK_DEFAULT_HZ, false, false);
    }
    // Internal stack: the task writes NVS.
    xTaskCreatePinnedToCore(cal_task_fn, "sndcal", 4096, NULL, PRIO_STORE, NULL, CORE_IO);
}
