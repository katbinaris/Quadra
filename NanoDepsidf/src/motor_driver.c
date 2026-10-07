#include "motor_driver.h"
#include "tasks_common.h"
#include "board_pins.h"
#include "motor_config.h"
#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "soc/mcpwm_struct.h"
#include <math.h>
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "motor_driver";

// Carrier frequency = resolution_hz / period_ticks (in UP_DOWN mode, one full up+down cycle
// takes exactly period_ticks ticks). Originally 10MHz/1000=10kHz, matching Espressif's
// mcpwm_foc_svpwm_open_loop example -- but 10kHz is right in the audible range and produced
// a persistent high-pitched whine from the motor windings. Was raised to 32MHz/32kHz
// (above human hearing, see git history/DEVELOPMENT_PLAN.md for the derivation) -- but that
// caused a real brownout under a tight 5V/500mA USB port supply at the time: confirmed by
// reverting to 10kHz alone (no other change) and the brownout stopped, even though modeled
// average motor current was already nowhere near the port's 500mA limit. Conclusion then:
// the 32kHz carrier's switching-current spikes (not average current) were tripping the
// port's overcurrent protection.
//
// RE-TESTED and RAISED BACK to 32kHz: clean on hardware this time, no brownout. Current
// draw context has changed a lot since the original incident (0.5A static cap now vs.
// whatever was in effect then, haptic-mode usage patterns instead of the original
// sustained PD-hold test, etc.), so this isn't necessarily proof the underlying physical
// risk (switching-current spikes on a weak supply) is gone -- it's supply-dependent and
// was only ever confirmed on one specific tight 5V/500mA USB port. Worth re-confirming on
// that same weak supply specifically if it's still around, rather than assuming today's
// clean result generalizes to every supply.
#define MCPWM_RESOLUTION_HZ 32000000 // 32MHz -> 32kHz carrier (see above)
#define MCPWM_PERIOD_TICKS  1000

// ACTUAL VALID COMPARE RANGE, confirmed by reading esp_driver_mcpwm's source directly
// (not assumed) after "compare value out of range" kept recurring despite a duty clamp
// that should have prevented it: mcpwm_timer.c halves period_ticks into peak_ticks for
// MCPWM_TIMER_COUNT_MODE_UP_DOWN ("in symmetric mode, peak_ticks = period_ticks / 2"), and
// mcpwm_cmpr.c's mcpwm_comparator_set_compare_value() validates against peak_ticks, not
// period_ticks. Every prior "out of range" incident across this bring-up was this same
// factor-of-2 mistake -- compare values were being scaled against MCPWM_PERIOD_TICKS (1000)
// when the real ceiling in this count mode is always half that.
#define MCPWM_PEAK_TICKS (MCPWM_PERIOD_TICKS / 2)

static mcpwm_timer_handle_t s_timer;
static mcpwm_oper_handle_t s_oper[3];
static mcpwm_cmpr_handle_t s_cmpr[3];
static mcpwm_gen_handle_t s_gen[3];

// --- Sound, at the PWM rate ---
// While a sound plays, an interrupt on every PWM period (32 kHz) writes the three compare
// values: where the loop wants the phases, plus one sample of the sound. The 10 kHz loop still
// decides everything, each tick (motor_driver_tone()): the phase voltages without the sound,
// each voice's pitch and level, and which way the d and the q axis point on the phases. The
// interrupt only steps the oscillators. (The loop alone could play up to 4 kHz: at 10 kHz a
// 4 kHz sine is 2.5 samples a cycle.)
// Integers only in there: the FPU isn't saved for interrupts (CONFIG_FREERTOS_FPU_IN_ISR is
// off). It is on for the length of a sound and off again, so it costs nothing otherwise.
#define MCPWM_CARRIER_HZ (MCPWM_RESOLUTION_HZ / MCPWM_PERIOD_TICKS)
#define TONE_SIN_BITS 8
#define TONE_SIN_SHIFT 14  // the sine table's 1.0 is 1 << this
#define TONE_VOLT_SHIFT 12 // levels and limits: volts << this
#define TONE_AXIS_SHIFT 8  // an axis on a phase: compare ticks per volt << this
#define TONE_CMP_MIN 5     // DUTY_MARGIN (below) of MCPWM_PEAK_TICKS
#define TONE_CMP_MAX (MCPWM_PEAK_TICKS - 5)
typedef struct {
    int32_t base[3];           // the compare values without the sound
    int32_t axis[2][3];        // 1 V on the d [0] and the q [1] axis, on each phase
    int32_t lim_min[2], lim_max[2]; // the summed sound is clipped to these, per axis
    struct {
        uint32_t inc; // phase step per PWM period, a full turn is 2^32
        int32_t amp;  // 0 = silent
        uint8_t q, wave;
    } voice[MOTOR_TONE_VOICES];
} tone_t;
static int16_t s_tone_sin[1 << TONE_SIN_BITS]; // filled at init: in RAM, where the interrupt can read it
static int16_t s_tone_rich[1 << TONE_SIN_BITS]; // MOTOR_TONE_RICH, its peak at the table's 1.0
// The loop writes the one not in use and then switches: both run on Core 0, the interrupt on
// top of the loop, so it never sees one half written.
static tone_t s_tone[2];
static volatile uint32_t s_tone_use = 0;
static uint32_t s_tone_phase[MOTOR_TONE_VOICES];
static int32_t s_tone_noise[MOTOR_TONE_VOICES]; // each noise voice's current cycle: 1 or -1
static uint32_t s_tone_rand = 0x2545F491;       // xorshift, never 0
static bool s_tone_on = false;
static uint32_t s_tone_int_mask = 0; // the timer's "count is zero" interrupt

static const int s_in_pins[3] = { PIN_IN_U, PIN_IN_V, PIN_IN_W };
static const int s_en_pins[3] = { PIN_EN_U, PIN_EN_V, PIN_EN_W };

void motor_driver_enable(bool enable) {
    for (int i = 0; i < 3; i++) {
        gpio_set_level(s_en_pins[i], enable ? 1 : 0);
    }
}

// Every PWM period while a sound plays. What it writes takes effect at the next period's start.
static bool CONTROL_HOT tone_on_period(mcpwm_timer_handle_t timer, const mcpwm_timer_event_data_t *edata, void *user) {
    const tone_t *t = &s_tone[s_tone_use];
    int32_t sum[2] = {0, 0};
    for (int v = 0; v < MOTOR_TONE_VOICES; v++) {
        if (t->voice[v].amp == 0) continue;
        uint32_t ph = s_tone_phase[v] + t->voice[v].inc;
        s_tone_phase[v] = ph;
        int32_t s;
        if (t->voice[v].wave == MOTOR_TONE_SINE) {
            s = s_tone_sin[ph >> (32 - TONE_SIN_BITS)];
        } else if (t->voice[v].wave == MOTOR_TONE_RICH) {
            s = s_tone_rich[ph >> (32 - TONE_SIN_BITS)];
        } else if (t->voice[v].wave == MOTOR_TONE_SQUARE) {
            s = (int32_t)ph < 0 ? -(1 << TONE_SIN_SHIFT) : (1 << TONE_SIN_SHIFT);
        } else {
            // Noise: whole cycles of the sine, each the right way up or upside down at random.
            // Every cycle averages zero, so there is nothing slow in it. (It was random
            // +-1 steps at first: their long runs are DC to the windings, which only
            // inductance keeps the current out of, and on the q axis they are torque. A click
            // made of them on the q axis pulled enough to make the USB host cut the power.)
            if (ph < t->voice[v].inc) { // the phase has just wrapped
                uint32_t r = s_tone_rand;
                r ^= r << 13, r ^= r >> 17, r ^= r << 5;
                s_tone_rand = r;
                s_tone_noise[v] = r & 1 ? 1 : -1;
            }
            s = s_tone_noise[v] * s_tone_sin[ph >> (32 - TONE_SIN_BITS)];
        }
        sum[t->voice[v].q] += (t->voice[v].amp * s) >> TONE_SIN_SHIFT;
    }
    for (int a = 0; a < 2; a++) {
        if (sum[a] > t->lim_max[a]) sum[a] = t->lim_max[a];
        if (sum[a] < t->lim_min[a]) sum[a] = t->lim_min[a];
    }
    const int shift = TONE_VOLT_SHIFT + TONE_AXIS_SHIFT;
    for (int i = 0; i < 3; i++) {
        int32_t c = t->base[i] + ((sum[0] * t->axis[0][i] + sum[1] * t->axis[1][i] + (1 << (shift - 1))) >> shift);
        if (c < TONE_CMP_MIN) c = TONE_CMP_MIN;
        if (c > TONE_CMP_MAX) c = TONE_CMP_MAX;
        mcpwm_comparator_set_compare_value(s_cmpr[i], (uint32_t)c);
    }
    return false;
}

esp_err_t motor_driver_init(void) {
    // NOTE on EN/FAULT: the STSPIN233's EN pins are technically combined EN/FAULT pins --
    // the chip can pull one low internally on a real overcurrent/short/thermal fault, and
    // ST's reference design pairs this with an external RC network for auto-retry timing.
    // An earlier version of this code switched these to open-drain + readable to make use
    // of that. Reverted: legacy Arduino/SimpleFOC firmware drove these same pins as plain
    // push-pull outputs (never reading them) and successfully spun this exact motor/board,
    // and with the open-drain change the fault-read was permanently stuck low, unchanging,
    // across a 10s test with zero commanded voltage -- consistent with a floating input
    // (no pull-up to release to) rather than a real fault signal. This board most likely
    // doesn't implement the external RC network the fault-reporting feature needs, so
    // there's no reliable way to read fault state here -- back to plain push-pull, matching
    // what's proven to work, relying on the voltage/current limits in motor_config.h and
    // the wall-clock hard timeout in control_task.c for safety instead.
    uint64_t en_mask = 0;
    for (int i = 0; i < 3; i++) {
        en_mask |= (1ULL << s_en_pins[i]);
    }
    gpio_config_t en_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = en_mask,
    };
    ESP_ERROR_CHECK(gpio_config(&en_cfg));
    motor_driver_enable(false); // stay disabled until PWM outputs are at a known-safe state

    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = MCPWM_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP_DOWN, // center-aligned, lower switching noise
        .period_ticks = MCPWM_PERIOD_TICKS,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &s_timer));
    // The sound's interrupt: registered here (the driver wants it before the timer is
    // enabled), on this task's core, and switched straight off again until a sound plays.
    for (int i = 0; i < (1 << TONE_SIN_BITS); i++) {
        s_tone_sin[i] = (int16_t)lroundf((1 << TONE_SIN_SHIFT) * sinf(i * (6.2831853f / (1 << TONE_SIN_BITS))));
    }
    float rich[1 << TONE_SIN_BITS], peak = 0.0f;
    for (int i = 0; i < (1 << TONE_SIN_BITS); i++) {
        float x = i * (6.2831853f / (1 << TONE_SIN_BITS));
        rich[i] = sinf(x) + 0.5f * sinf(2.0f * x) + 0.25f * sinf(3.0f * x);
        if (fabsf(rich[i]) > peak) peak = fabsf(rich[i]);
    }
    for (int i = 0; i < (1 << TONE_SIN_BITS); i++) s_tone_rich[i] = (int16_t)lroundf((1 << TONE_SIN_SHIFT) * rich[i] / peak);
    mcpwm_timer_event_callbacks_t tone_cbs = { .on_empty = tone_on_period };
    ESP_ERROR_CHECK(mcpwm_timer_register_event_callbacks(s_timer, &tone_cbs, NULL));
    s_tone_int_mask = MCPWM0.int_ena.val;
    MCPWM0.int_ena.val = 0;

    for (int i = 0; i < 3; i++) {
        mcpwm_operator_config_t oper_config = { .group_id = 0 };
        ESP_ERROR_CHECK(mcpwm_new_operator(&oper_config, &s_oper[i]));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_oper[i], s_timer));

        mcpwm_comparator_config_t cmpr_config = { .flags.update_cmp_on_tez = true };
        ESP_ERROR_CHECK(mcpwm_new_comparator(s_oper[i], &cmpr_config, &s_cmpr[i]));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(s_cmpr[i], 0));

        mcpwm_generator_config_t gen_config = { .gen_gpio_num = s_in_pins[i] };
        ESP_ERROR_CHECK(mcpwm_new_generator(s_oper[i], &gen_config, &s_gen[i]));

        // Single generator per phase -- same edge convention (UP+compare->LOW,
        // DOWN+compare->HIGH) as Espressif's validated reference example's high-side
        // generator, just without a paired complementary/dead-time generator.
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_gen[i],
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_cmpr[i], MCPWM_GEN_ACTION_LOW)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(s_gen[i],
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, s_cmpr[i], MCPWM_GEN_ACTION_HIGH)));
    }

    ESP_ERROR_CHECK(mcpwm_timer_enable(s_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP));

    ESP_LOGI(TAG, "motor driver initialized (PWM running, EN outputs disabled)");
    return ESP_OK;
}

// Clamp with margin, not exactly [0,1] -- a correctly-derived command can still touch the
// exact duty=0 or duty=1 boundary at sinusoid peaks (e.g. cos(0)=1 precisely), and that
// exact boundary value was rejected as "out of range" by mcpwm during first bring-up (see
// motor_config.h). Structural fix: never let the compare value get near either edge,
// regardless of what voltage upstream code requests.
#define DUTY_MARGIN 0.01f

static uint32_t CONTROL_HOT voltage_to_compare(float phase_volts) {
    // This board's driver switches each phase between 0V and the supply rail, so duty=0.5
    // means 0V average -- duty fraction is referenced to a virtual neutral at Vbus/2.
    float duty = (phase_volts / MOTOR_MAX_VOLTAGE_V) + 0.5f;
    if (duty < DUTY_MARGIN) duty = DUTY_MARGIN;
    if (duty > 1.0f - DUTY_MARGIN) duty = 1.0f - DUTY_MARGIN;
    return (uint32_t)(duty * MCPWM_PEAK_TICKS); // NOT MCPWM_PERIOD_TICKS -- see comment above
}

void CONTROL_HOT motor_driver_set_phase_voltages(float ua, float ub, float uc) {
    if (s_tone_on) {
        MCPWM0.int_ena.val = 0; // nothing else of MCPWM0's interrupts is in use
        s_tone_on = false;
    }
    mcpwm_comparator_set_compare_value(s_cmpr[0], voltage_to_compare(ua));
    mcpwm_comparator_set_compare_value(s_cmpr[1], voltage_to_compare(ub));
    mcpwm_comparator_set_compare_value(s_cmpr[2], voltage_to_compare(uc));
}

void CONTROL_HOT motor_driver_tone(const motor_tone_t *in) {
    const float volt = (float)(1 << TONE_VOLT_SHIFT);
    const float axis = (1 << TONE_AXIS_SHIFT) * MCPWM_PEAK_TICKS / MOTOR_MAX_VOLTAGE_V;
    tone_t *t = &s_tone[s_tone_use ^ 1];
    for (int i = 0; i < 3; i++) {
        t->base[i] = (int32_t)voltage_to_compare(in->u[i]);
        t->axis[0][i] = (int32_t)(in->d[i] * axis);
        t->axis[1][i] = (int32_t)(in->q[i] * axis);
    }
    t->lim_min[0] = (int32_t)(-in->d_lim * volt);
    t->lim_max[0] = (int32_t)(in->d_lim * volt);
    t->lim_min[1] = (int32_t)(in->q_min * volt);
    t->lim_max[1] = (int32_t)(in->q_max * volt);
    for (int v = 0; v < MOTOR_TONE_VOICES; v++) {
        const motor_tone_voice_t *vc = &in->voice[v];
        // Under 16 kHz (half the PWM rate) the step fits a signed word, and the loop has no
        // unsigned conversion to call.
        t->voice[v].inc = (uint32_t)(int32_t)(vc->hz * (4294967296.0f / MCPWM_CARRIER_HZ));
        t->voice[v].amp = (int32_t)(vc->volts * volt);
        t->voice[v].q = vc->q;
        t->voice[v].wave = vc->wave;
        if (vc->start) {
            s_tone_phase[v] = vc->cosine ? 0x40000000u : 0u;
            s_tone_noise[v] = 1;
        }
    }
    s_tone_use ^= 1;
    if (!s_tone_on) {
        MCPWM0.int_clr.val = s_tone_int_mask;
        MCPWM0.int_ena.val = s_tone_int_mask;
        s_tone_on = true;
    }
}
