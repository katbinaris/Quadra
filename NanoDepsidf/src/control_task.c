#include "control_task.h"
#include "tasks_common.h"
#include "ipc.h"
#include "mt6701.h"
#include "motor_config.h"
#include "motor_driver.h"
#include "foc_math.h"
#include "foc_calibration.h"
#include "board_pins.h"
#include "motor_sound.h"
#include "ui_state.h"
#include "menu.h"
#include "haptic_params.h"
#include "app_mode.h"
#include "sysmon.h"
#include "notify.h"
#include "ext_link.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "home.h"
#include "midi.h"
#include "driver/gptimer.h"
#include <math.h>
#include <stdbool.h>
#include <inttypes.h>

static const char *TAG = "control";
static TaskHandle_t s_task_handle = NULL;
static gptimer_handle_t s_pacing_timer = NULL;

// Converts a duration in milliseconds to an iteration count at the current
// CONTROL_LOOP_PERIOD_US, so every *_ITERS constant below keeps its real-world meaning
// regardless of loop rate (see the comment on CONTROL_LOOP_PERIOD_US in tasks_common.h).
#define MS_TO_ITERS(ms) ((uint32_t)(((uint64_t)(ms) * 1000ULL) / CONTROL_LOOP_PERIOD_US))
// Inverse of the above, for turning an iteration count back into seconds for logging.
#define ITERS_TO_SEC(iters) (((iters) * (float)CONTROL_LOOP_PERIOD_US) / 1000000.0f)

// --- Phase 2a closed-loop bench validation ---
// Open-loop bring-up proved the driver/sensor/commutation are electrically correct (up to
// 1000 RPM). A first closed-loop position-hold test then proved calibration + real feedback
// control work (bidirectional holding confirmed by hand). This extends that into a bench
// validation: step through several target positions around a full revolution (not just
// holding wherever the rotor happened to start), measuring steady-state tracking accuracy
// at each, using a PD controller (P alone showed real but expected overshoot on a firm
// nudge -- D damps that). NOT the real haptic control program -- that's Phase 2b, gated on
// a joint design session.
//
// New risk closed-loop introduces that open-loop didn't have: a sign error in calibration
// (direction or offset) turns the controller into POSITIVE feedback -- it would push harder
// away from the target as error grows, instead of correcting toward it. Voltage is still
// capped the same way as every other test (see motor_config.h), so this isn't a new
// electrical-safety issue, but it is a real correctness failure mode -- guarded by
// CL_DIVERGE_ABORT_RAD below.
typedef enum { CL_HOLD, CL_RAMP_DOWN, CL_DONE } closed_loop_phase_t;
static closed_loop_phase_t s_cl_phase = CL_DONE; // CL_DONE until calibration succeeds
static uint32_t s_cl_iter = 0;
static uint32_t s_cl_target_idx = 0;
static foc_calibration_t s_cal;
static float s_base_mech_rad = 0.0f;
static float s_last_vq = 0.0f;
static float s_applied_vq = 0.0f; // haptic mode's q-axis voltage last sent to the driver (SYS INFO)
static float s_applied_vd = 0.0f; // and the d axis: the motor's sound (motor_sound.h)
static float s_prev_mech_rad = 0.0f;
static bool s_prev_mech_rad_valid = false;

// Steady-state tracking stats, accumulated over the last CL_STEADYSTATE_ITERS of each
// target's hold (after the initial move/settle transient) and reset per target.
static float s_ss_max_abs_error = 0.0f;
static float s_ss_sum_abs_error = 0.0f;
static uint32_t s_ss_count = 0;

// Tour around a full revolution and back, relative to wherever the rotor sits when the test
// starts -- validates arbitrary commanded positions, not just "hold the starting point".
// RESTORED from the single-position PD-tuning simplification: 0.35A/0.5A static-cap bisection
// (single fixed position, gentle) came back clean at the full 0.5A, matching the rotating
// cap and legacy's target. Real next question: does the ORIGINAL brownout (multi-target
// tour, instant 90deg jumps) now also survive at 0.5A, now that voltage slew-rate limiting
// (MOTOR_VOLTAGE_SLEW_LIMIT_V_PER_S, CL_MAX_VQ_STEP_V) is in place to bound the inrush a
// jump like that causes? If yes, slew-limiting was the real fix all along, not a lower
// static cap. 3s/target restored (shorter than the 60s single-position test -- this is
// re-testing the tour, not interactive push-testing).
//
// EXTENDED to 30 evenly-spaced targets (12deg apart) around a full revolution, per request,
// to test haptics between much smaller steps than the original 5-point (90deg) tour --
// computed at runtime (not a 30-element literal array) since the values are a trivial
// closed form. 1.5s hold/target (30 x 1.5s = 45s total, still bounded).
#define CL_NUM_TARGETS 30
static float s_cl_target_offsets[CL_NUM_TARGETS];
#define CL_TARGET_HOLD_ITERS MS_TO_ITERS(1500) // 1.5s per target: move + settle + measure
#define CL_STEADYSTATE_ITERS MS_TO_ITERS(500) // last 0.5s of each target's hold used for jitter stats

#define CL_RAMP_DOWN_ITERS MS_TO_ITERS(300) // 0.3s soft-stop

// PD gains. Kp alone (first closed-loop test) held correctly but overshot visibly on a firm
// nudge -- textbook proportional-only behavior. Kd damps velocity to tighten that up.
//
// RAISED to 5.0/0.08 for the 30-target (12deg step) tour to force movement through what
// looked like static-friction/cogging breakaway torque (target 7/30 frozen for a full
// 1.5s hold). REVERTED back down: that symptom is now understood to have actually been the
// MOTOR_POLE_PAIRS bug (was 4, corrected to 7 -- see motor_config.h) -- with the wrong pole
// count, commutation phase alignment only matched reality near the calibration point and
// drifted at other mechanical positions, producing exactly this kind of "stuck at one
// particular target" pattern, not real friction. Now that phase alignment is correct
// everywhere, real torque-per-volt is both stronger and consistent at every position, so
// the boosted gains overshoot/oscillate violently on a target step -- confirmed on hardware
// immediately after the pole-pair fix. Back to the original pre-boost values as the sane
// starting point; retest the single-position hold and 30-target tour, and only raise again
// incrementally (watching for overshoot/ringing) if still needed now that the real root
// cause is fixed.
#define CL_KP 1.5f  // V/rad
#define CL_KD 0.05f // V per (rad/s)

// Max commanded-voltage change per control iteration, derived from
// MOTOR_VOLTAGE_SLEW_LIMIT_V_PER_S -- bounds inrush current on a target step (a legitimate
// up-to-180deg position change otherwise commands close to full effective voltage in one
// 1ms tick, which the static current cap's steady-state Ohm's-law model can't account for).
#define CL_MAX_VQ_STEP_V (MOTOR_VOLTAGE_SLEW_LIMIT_V_PER_S * (CONTROL_LOOP_PERIOD_US / 1000000.0f))

#define CL_DIVERGE_ABORT_RAD 1.5f // ~86 deg -- if a converged hold needs this much
                                  // correction, treat it as a likely sign error
#define CL_DIVERGE_CHECK_SETTLE_ITERS MS_TO_ITERS(1000) // 1s grace period after each target change before
                                            // the check above applies -- a legitimate target
                                            // step (up to 180 deg) creates a large error for
                                            // an instant, before the motor has had time to
                                            // move; checking from tick 0 of a new target
                                            // false-tripped on exactly that (bug found during
                                            // the first bench-validation run, not a real
                                            // calibration/control problem -- target 1 held to
                                            // <0.01 rad both times before this fired)

#define OL_ENABLE_NEUTRAL_ITERS MS_TO_ITERS(300) // 0.3s at 0V before calibration starts

// --- Reconstructed open-loop diagnostic (BTN_C at boot) ---
// The original open-loop rotation test from earlier this session's bring-up (proven strong,
// no brownouts, up to 1000 RPM) was fully replaced by the closed-loop path, not merely
// disabled -- and NanoDepsidf/ was never under git, so there's no history to restore it
// from. Reconstructed here as a separate BTN_C-gated mode specifically so it runs on the
// *exact same* build (same MOTOR_MAX_CURRENT_*, same 10kHz carrier, same hardware) as the
// closed-loop test that's currently weak/browning out -- an apples-to-apples check of
// whether that's a closed-loop-specific regression or something affecting the driver at a
// more fundamental level (hardware, caps, PWM). Deliberately simpler than the original
// 5-step 1000 RPM staircase (this is a quick diagnostic, not a re-run of full bring-up):
// one ramp to a modest 200 RPM (a level the original test held strongly at), brief hold,
// ramp down. Uses the direct-axis (Vd) trick like the original -- no calibration/feedback
// needed for open-loop forced commutation.
typedef enum { OL_RAMP, OL_HOLD, OL_RAMPDOWN, OL_TEST_DONE } open_loop_phase_t;
static open_loop_phase_t s_ol_phase = OL_TEST_DONE;
static uint32_t s_ol_iter = 0;
static float s_ol_theta_e = 0.0f;
static int64_t s_ol_start_us = 0;
static bool s_open_loop_mode = false;

#define OL_START_RPM 20.0f
#define OL_TARGET_RPM 200.0f // modest vs. the original's 1000 RPM ceiling -- this level held
                             // strongly with no brownout earlier in bring-up, enough to
                             // answer "is the driver/hardware still fine" quickly
#define OL_SPEED_RAMP_ITERS MS_TO_ITERS(5000) // 5s ramp, same reasoning as the original: an instant
                                 // step risks the rotor falling out of sync (open-loop has
                                 // no feedback to correct a missed step)
#define OL_ROTATE_HOLD_ITERS MS_TO_ITERS(5000) // 5s hold at target RPM
#define OL_RAMPDOWN_ITERS_2 MS_TO_ITERS(300) // 0.3s soft-stop (separate name from CL_RAMP_DOWN_ITERS)
#define OL_HARD_TIMEOUT_US (15 * 1000 * 1000) // 15s, above the ~10.3s expected duration

// --- Pole-pair verification diagnostic (BTN_D at boot) ---
// The "properly unconfounded" method noted in motor_config.h's history, finally wired up:
// command a KNOWN, FIXED electrical frequency open-loop (sidesteps MOTOR_POLE_PAIRS
// entirely -- driving directly in electrical Hz, not RPM, means this test doesn't depend on
// the value it's trying to verify), then measure the ACTUAL mechanical rotation via the
// MT6701 sensor over a fixed window. pole_pairs = electrical_Hz / measured_mechanical_Hz.
// Diagnostic only -- does not change MOTOR_POLE_PAIRS itself, just logs the computed value
// alongside the currently-configured one for comparison.
typedef enum { PP_RAMP, PP_MEASURE, PP_RAMPDOWN, PP_DONE } polepair_test_phase_t;
static polepair_test_phase_t s_pp_phase = PP_DONE;
static uint32_t s_pp_iter = 0;
static float s_pp_theta_e = 0.0f;
static float s_pp_prev_mech_rad = 0.0f;
static bool s_pp_prev_mech_rad_valid = false;
static float s_pp_accumulated_mech_rad = 0.0f;
static bool s_polepair_test_mode = false;
static int64_t s_pp_start_us = 0;

#define PP_TEST_ELECTRICAL_HZ 3.0f // modest -- low slip risk across the plausible 4-7 pole-
                                   // pair range this is meant to distinguish between
#define PP_RAMP_ITERS MS_TO_ITERS(2000) // 2s ramp to PP_TEST_ELECTRICAL_HZ -- avoids an instant-step
                            // slip risk, same reasoning as the open-loop diagnostic's ramp
#define PP_MEASURE_ITERS MS_TO_ITERS(5000) // 5s at constant electrical Hz -- mechanical angle accumulated
                              // over this window via the sensor
#define PP_RAMPDOWN_ITERS MS_TO_ITERS(300) // 0.3s soft-stop
#define PP_HARD_TIMEOUT_US (12 * 1000 * 1000) // 12s, above the ~7.3s expected duration

// --- Haptic detent demo (BTN_A+BTN_B+BTN_D at boot) ---
// Nearest-grid-point linear ("sawtooth") detent profile: retarget every tick to whichever
// of the N evenly-spaced detents is nearest, then Vq = Kp*error - Kd*velocity. Force ramps
// UP linearly as you move away from a detent center, then INSTANTLY inverts sign at the
// midpoint between two detents -- the max-slope moment (the snap) happens exactly at the
// boundary, and the profile is gentlest exactly at rest (center).
//
// SECOND APPROACH after a sine-wave profile (Vq = -Kclick*sin(N*rel) - Kd*velocity)
// prototype: continuous, no target-flip discontinuity, but structurally backwards for a
// "click" -- sine's slope (local stiffness) is STEEPEST at the center (causing persistent
// oscillation there, worse the more detents/gain) and FLATTEST at the boundary (the
// opposite of a snap -- felt like "a bump" not a click, no matter how much Kp/Kd/filtering/
// loop-rate tuning was applied). This linear profile has the max-slope moment in the right
// place (the boundary) and is gentler at the exact point (center) that was ringing.
//
// FIRST tried this same linear approach even earlier, before most of the infrastructure
// improvements landed (10kHz loop, rate-correct velocity filter, 0.5A current cap, faster
// haptic-only slew, legacy-inspired velocity coasting) -- it was never fairly retested
// under today's conditions, only compared against sine under the old, weaker setup.
// HAPTIC_NUM_DETENTS_*/HAPTIC_KP_*/HAPTIC_KD_* (default/min/max) now live in haptic_params.h,
// shared with menu.c's Haptic Configurator screen -- see that header for why. The velocity
// damping "viscous fluid" feel noted below (Kp=HAPTIC_KP_MIN, Kd~0.055) falls straight out of
// those same two live-tunable knobs, no separate mode needed.
//
// Extra margin (fraction of one detent spacing) past the midpoint before the committed
// detent switches. Without this, sitting still exactly at a midpoint lets sensor noise
// alone flip the nearest-detent pick every tick, chattering between two targets.
#define HAPTIC_DETENT_HYSTERESIS_FRAC 0.15f
// End stops: a list at its end (the command wheel, the PROFILE carousel) turns the next detent
// into a wall -- the committed detent is kept, with no click and no step, and the spring
// keeps pulling back to it. Past the point where a detent would normally switch, stiffness
// ramps up by HAPTIC_WALL_GAIN x Kp on top of the normal spring, starting from zero there so
// the force is continuous (no bump). No breakaway: the first version re-based after 1.6
// detents, which felt like a ratchet on hardware ("pushes back, then a noticeable skip"). The
// push is still capped by the motor voltage limit, so a hand can overpower it; let go and
// it springs back to the end detent.
#define HAPTIC_WALL_GAIN 3.0f
// Compared against legacy_fw/src/haptic.cpp (SimpleFOC): the dominant reason legacy feels
// "clicky" while ours feels "dampened" is that legacy has real closed-loop current control
// (up to 1.22A/5V) -- ~6x our static Ohm's-law voltage/current ceiling (~0.3A/0.79V), a hard
// physical limit no amount of Kp/Kd retuning can work around. But legacy also does two
// things structurally different that ARE worth adopting without touching the current cap:
// (1) it zeroes the restoring torque entirely above a shaft-velocity threshold ("knob is
// smooth while rotating quickly by hand but snappy during fine adjust" -- their comment) --
// our -Kd*velocity term does the opposite, actively resisting fast motion, which reads as
// friction; (2) it has no explicit slew limit, relying on SimpleFOC's real current control
// to respond in one PWM cycle, vs. our explicit rate limit smearing the boundary flip out
// over real time even within our already-small voltage budget.
#define HAPTIC_COAST_VELOCITY_RAD_S 30.0f // legacy's threshold -- above this speed, apply
                                          // ZERO torque (neither Kp nor Kd) and let the
                                          // knob coast freely on hand momentum; only engage
                                          // the restoring spring for slow/fine adjustment
#define HAPTIC_VELOCITY_FILTER_TAU_S 0.0067f // EMA low-pass time constant on velocity before
                                             // it feeds Kd -- raw finite-difference velocity
                                             // from a quantized 14-bit encoder is noisy;
                                             // that noise is normally masked by a large
                                             // steady Kp*error torque, but right at a
                                             // detent center (error near zero) the
                                             // restoring term is near zero too, so
                                             // unfiltered noise through Kd was the
                                             // dominant, unmasked signal -- causing the
                                             // buzzing/oscillation-only-at-center symptom
                                             // found on hardware. Expressed as a real time
                                             // constant (not a bare alpha) so it stays
                                             // correct if CONTROL_LOOP_PERIOD_US changes --
                                             // a fixed alpha would silently get 10x weaker
                                             // in real-world terms when the loop rate went
                                             // 1kHz->10kHz (tau = dt/alpha, so the same
                                             // alpha at a 10x smaller dt is a 10x shorter,
                                             // i.e. weaker, filter) -- exactly the kind of
                                             // bug that motivated MS_TO_ITERS() elsewhere.
                                             // 0.0067s matches the originally-tuned alpha=
                                             // 0.15 at the old 1kHz rate. Lower = more
                                             // filtering.
#define HAPTIC_VELOCITY_FILTER_ALPHA \
    ((CONTROL_LOOP_PERIOD_US / 1000000.0f) / (HAPTIC_VELOCITY_FILTER_TAU_S + (CONTROL_LOOP_PERIOD_US / 1000000.0f)))
#define HAPTIC_VOLTAGE_SLEW_LIMIT_V_PER_S 200.0f // LOOSENED from 40 -- still bounded (a
                                                 // literal instant flip risks an inrush
                                                 // spike the static Ohm's-law cap can't see),
                                                 // but legacy's real current control responds
                                                 // far faster than our original slew rate
#define HAPTIC_MAX_VQ_STEP_V (HAPTIC_VOLTAGE_SLEW_LIMIT_V_PER_S * (CONTROL_LOOP_PERIOD_US / 1000000.0f))
// "Click" transient pulse: on detent-index change, inject a short decaying oscillation
// directly on top of the (slew-limited) background Vq, DELIBERATELY bypassing the slew
// limiter -- the whole point is a fast, audible tick, and the slew limiter's job is to
// smooth exactly that kind of transition, so the two goals are in direct conflict. Still
// hard-clamped to MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V same as everything else, so it
// can't exceed the current-safety model -- just approach that same cap much faster than
// the slew limiter would normally allow. Residual risk worth watching for on hardware: a
// clipped +-cap oscillation is a smaller-scale version of the same switching-current-spike
// phenomenon that caused the 32kHz-PWM-carrier brownout earlier in this project's history
// -- much gentler here (200Hz vs 32kHz, ~160x slower edge rate) but not rigorously zero.
// Two-stage: a brief, high-amplitude/lower-frequency IMPACT (deliberately over-driven past
// the voltage cap so it clips into a sharp edge -- that clipping is what gives it a
// percussive feel rather than a tone), followed by a longer, lower-amplitude/lower-
// frequency TAIL that rings down smoothly (mostly stays under the cap, so it's a cleaner
// sine, mimicking a real detent's decaying mechanical ring after the initial impact).
//
// TUNED for a deeper "thock" instead of a sharp/buzzy "tick": both stages' frequencies
// dropped a lot (600->100Hz impact, 150->70Hz tail) and the impact stretched slightly
// (2->4ms) for more perceived weight -- lower frequency content reads as duller/heavier,
// higher as sharper/buzzier, independent of amplitude/clipping.
#define HAPTIC_PULSE_IMPACT_DURATION_ITERS MS_TO_ITERS(4) // 4ms low, punchy impact
#define HAPTIC_PULSE_IMPACT_FREQ_HZ 100.0f
#define HAPTIC_PULSE_IMPACT_AMPLITUDE_V 6.0f // well above the ~1.3V cap on purpose -- clips
                                             // hard into a near-square edge
#define HAPTIC_PULSE_TAIL_DURATION_ITERS MS_TO_ITERS(20) // 20ms softer ring-down
#define HAPTIC_PULSE_TAIL_FREQ_HZ 70.0f
#define HAPTIC_PULSE_TAIL_AMPLITUDE_V 1.0f // under the cap -- stays a mostly-clean sine
#define HAPTIC_PULSE_DURATION_ITERS (HAPTIC_PULSE_IMPACT_DURATION_ITERS + HAPTIC_PULSE_TAIL_DURATION_ITERS)
// No HAPTIC_RUN_DURATION_US/HARD_TIMEOUT_US -- deliberately runs indefinitely, see the
// comment in the main loop where the other modes' hard-timeout checks live.

// Phase 3: first mapping-engine decision made (see DEVELOPMENT_PLAN.md "Open decisions")
// -- knob rotation maps to the HID mouse scroll wheel, one detent crossing = one step.
// +1 = one sign of rotation, -1 = the other; which physical direction (CW/CCW) that
// actually is depends on this board's mounting/calibration and hasn't been checked on
// hardware yet -- flip this single constant if scrolling comes out backwards, same
// pattern as MOTOR_POLE_PAIRS/s_cal.direction elsewhere in this file.
#define HID_WHEEL_SIGN 1
// Which physical turn counts as "positive" everywhere the knob means something -- menu, APP
// mode (detents, drags, end stops), the scroll wheel, the display's shape. -1 = inverted
// (user, 2026-09-30: the device now sits horizontally). Only the meaning flips: the motor,
// haptics and detent physics stay in sensor coordinates.
#define KNOB_DIRECTION (-1)

typedef enum { HAPTIC_RUN, HAPTIC_DONE } haptic_phase_t;
static haptic_phase_t s_haptic_phase = HAPTIC_DONE;
static int64_t s_haptic_start_us = 0;
static bool s_haptic_mode = false;

// Phase 8: the old live-tuning button combos described above (BTN_D/BTN_C detent count,
// BTN_A/BTN_B Kp, BTN_A+BTN_C/BTN_B+BTN_D Kd) are RETIRED -- F1/F3/F4 (BTN_A/BTN_C/BTN_D)
// now have fixed, global menu roles (select/back/open, see DEVELOPMENT_PLAN.md Phase 8 and
// the Architecture decisions log) that structurally conflict with the old per-combo
// meanings. Kp/Kd/detent-count are now live-adjustable through the real menu instead
// (Phase 8 step 3): read directly from menu.c's atomics (menu_get_haptic_kp() etc., cached
// once per tick into locals below) rather than duplicated as separate state here -- menu.c
// is the one place that already needs cross-core-safe live values (it also persists them via
// config_store.c/NVS and restores them at boot in menu_init(), before this task even starts).
static float s_haptic_filtered_velocity = 0.0f;
static int32_t s_haptic_prev_detent_index = 0;
static bool s_haptic_prev_detent_index_valid = false;
static uint32_t s_haptic_pulse_ticks_remaining = 0;
static float s_haptic_pulse_sign = 1.0f;

// F1-F4 (BTN_A-BTN_D) press-edge state for the real menu (`menu.c`). F2/BTN_B was reserved
// in Phase 8 and is Save since the Pixel UI (menu_input_save()). One shared cooldown (not
// per-button) is enough now that each button fires a single, unambiguous action rather than
// needing combo disambiguation like the retired scheme above did.
static bool s_notice_shown = false; // an agent notification is up (notify.h), menu closed
static uint8_t s_notice_keys = 0;    // keys pressed while one was up: hidden until released
static uint8_t s_notice_raw_prev = 0;
static bool s_menu_prev_btn_a_pressed = false;
static bool s_menu_prev_btn_b_pressed = false;
static bool s_menu_prev_btn_c_pressed = false;
static bool s_menu_prev_btn_d_pressed = false;
static uint32_t s_menu_btn_cooldown_until_iter = 0;
static uint32_t s_f2_hold_since_iter = 0; // F2 down on the Haptics screen, waiting for release or 1.5 s
#define MENU_BTN_COOLDOWN_ITERS MS_TO_ITERS(250) // 250ms, same debounce margin the retired scheme used

// Independent safety net, decoupled from the iteration counter above. First bring-up hit a
// real bug (see motor_config.h) where per-tick error logging starved the scheduler badly
// enough that the iteration count fell ~18s behind wall-clock time -- the motor stayed
// energized far longer than the intended bounded test because phase transitions were gated
// on iteration count, not actual elapsed time. This checks wall-clock time directly via
// esp_timer_get_time() and force-disables the driver past a hard deadline regardless of
// what the state machine above thinks is happening.
#define CL_HARD_TIMEOUT_US (60 * 1000 * 1000) // 60s, above the intended ~45.3s (30 x 1.5s)
static int64_t s_cl_start_us = 0;

static bool IRAM_ATTR pacing_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg) {
    // A gptimer interrupt on Core 0 that wakes the control task directly. It was an esp_timer
    // callback: the esp_timer ISR woke the esp_timer task, which woke this one -- two context
    // switches per tick, 10000 times a second. The per-tick work itself stays in the task
    // (blocking SPI reads, xQueueSend), only the wake-up is in the ISR.
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task_handle, &woken);
    return woken == pdTRUE;
}

// Wrap to (-pi, pi].
static float CONTROL_HOT wrap_pi(float rad) {
    while (rad > (float)M_PI) rad -= 2.0f * (float)M_PI;
    while (rad <= -(float)M_PI) rad += 2.0f * (float)M_PI;
    return rad;
}

static float CONTROL_HOT raw_to_rad(int32_t raw) {
    return ((float)raw / 16384.0f) * 2.0f * (float)M_PI;
}

// One detent's worth of turning, wherever it goes: the menu (list navigation, or a value while
// editing -- never a wheel event then), APP mode, HOME (home.h), MIDI (midi.h), or the mouse wheel. The knob's own crossings
// and the companion's turns (EXT_CMD_INPUT) both come through here.
static void CONTROL_HOT dispatch_turn(int8_t dir, bool app_on) {
    ui_state_note_turn(dir);
    if (menu_is_open()) {
        menu_input_rotate(dir);
    } else if (app_on) {
        app_mode_detent(dir, esp_timer_get_time());
    } else if (menu_get_hid_type() == MENU_HID_HOME) {
        home_input_rotate(dir);
    } else if (menu_get_hid_type() == MENU_HID_MIDI) {
        midi_input_rotate(dir);
    } else {
        // Phase 3: knob -> mouse scroll wheel. Non-blocking -- a full queue just drops this
        // event (counted for SYS INFO).
        hid_report_msg_t hid_msg = {.type = HID_EVENT_MOUSE_WHEEL, .wheel_delta = (int8_t)(dir * HID_WHEEL_SIGN)};
        if (xQueueSend(g_hid_report_queue, &hid_msg, 0) != pdTRUE) sysmon_note_hid_drop();
    }
}

static void CONTROL_HOT control_task_fn(void *arg) {
    ESP_LOGI(TAG, "control task started on core %d, prio %d", xPortGetCoreID(), uxTaskPriorityGet(NULL));

    // Deliberate delay before anything time-sensitive (button check, motor init) so there's
    // generous slack for a serial monitor to reattach after a physical unplug/replug --
    // macOS/host USB-CDC re-enumeration plus a monitor script's reconnect can easily eat
    // the first couple seconds after reset, which was repeatedly causing the actual
    // arm-check and test-run log lines to be missed entirely during bring-up.
    ESP_LOGI(TAG, "Waiting 3s before BTN_A arm check (attach a serial monitor now if you want to watch)...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_err_t sensor_err = mt6701_init();
    if (sensor_err != ESP_OK) {
        ESP_LOGE(TAG, "MT6701 init failed, angle reads will fail");
    }

    // The haptic detent demo (see haptic_phase_t below) is now the default/main behavior
    // on a plain reset/reconnect -- promoted from behind a button combo now that it's
    // mature/hardware-validated (see DEVELOPMENT_PLAN.md Phase 2b). This is a deliberate
    // departure from the earlier "motor never energizes on a plain reset" safety posture
    // established during Phase 2a bring-up (see DEVELOPMENT_PLAN.md: every reflash/reset
    // silently re-energizing the motor is exactly what made those early incidents hard to
    // investigate) -- that posture was about protecting an *unvalidated* control loop
    // during bring-up, not a blanket rule; haptic mode has its own independent safety net
    // (aborts if error ever exceeds one full detent spacing -- a real control bug, not
    // normal operation) and is exactly the mode meant to run continuously/hands-on.
    // BTN_A held at boot instead arms the legacy diagnostic suite: BTN_B held at the same
    // time forces a fresh calibration even if a valid one is cached in NVS; BTN_C held at
    // the same time selects the reconstructed open-loop diagnostic instead of the
    // closed-loop bench test (see open_loop_phase_t above); BTN_D held at the same time
    // selects the pole-pair verification diagnostic instead (see polepair_test_phase_t
    // above). The old explicit BTN_A+BTN_B+BTN_D combo for haptic mode is retired now that
    // it doesn't need a combo at all.
    gpio_config_t btn_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B) | (1ULL << PIN_BTN_C) | (1ULL << PIN_BTN_D),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn_cfg);
    // Poll for a few seconds rather than sampling once ~20ms after boot -- a single-shot
    // sample requires catching an exact split-second right at boot, which is impractical
    // when arming via unplug/replug (no way to see the log in real time to know when that
    // window is). This gives a few seconds of slack: hold BTN_A any time during the window.
    bool armed = false;
    bool force_recal = false;
    int last_level = -1;
    for (int i = 0; i < 60; i++) { // 60 x 50ms = 3s
        last_level = gpio_get_level(PIN_BTN_A);
        if (last_level == 0) { // assumed active-low; see diagnostic log below if this never arms
            armed = true;
            force_recal = (gpio_get_level(PIN_BTN_B) == 0);
            s_open_loop_mode = (gpio_get_level(PIN_BTN_C) == 0);
            s_polepair_test_mode = (gpio_get_level(PIN_BTN_D) == 0);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    // Haptic mode is now the unconditional default whenever the legacy diagnostic suite
    // isn't armed -- see the comment above this arm-window loop.
    s_haptic_mode = !armed;
    ESP_LOGI(TAG, "BTN_A arm window done, armed=%d, force_recal=%d, open_loop_mode=%d, polepair_test_mode=%d, haptic_mode=%d, last observed level=%d",
             armed, force_recal, s_open_loop_mode, s_polepair_test_mode, s_haptic_mode, last_level);

    {
        esp_err_t drv_err = motor_driver_init();
        if (drv_err != ESP_OK) {
            ESP_LOGE(TAG, "motor driver init failed, skipping test");
        } else {
            motor_driver_set_phase_voltages(0.0f, 0.0f, 0.0f); // neutral duty before enabling
            motor_driver_enable(true);
            s_cl_start_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(OL_ENABLE_NEUTRAL_ITERS));

            if (armed && s_open_loop_mode) {
                s_ol_phase = OL_RAMP;
                s_ol_start_us = esp_timer_get_time();
                ESP_LOGI(TAG, "ARMED: open-loop diagnostic, ramp %.0f->%.0f RPM over %.1fs, hold %.1fs",
                         OL_START_RPM, OL_TARGET_RPM, ITERS_TO_SEC(OL_SPEED_RAMP_ITERS), ITERS_TO_SEC(OL_ROTATE_HOLD_ITERS));
            } else if (s_haptic_mode) {
                // Reuses the calibration cache like the plain bench tour -- force_recal only
                // applies to the legacy diagnostic suite (BTN_A held), not the default path.
                bool have_cal = foc_calibration_load(&s_cal);
                if (!have_cal) {
                    s_cal = foc_calibration_run();
                    if (s_cal.valid) {
                        foc_calibration_save(&s_cal);
                    }
                }
                if (!s_cal.valid) {
                    ESP_LOGE(TAG, "calibration failed -- disabling, not attempting haptic demo");
                    motor_driver_enable(false);
                } else {
                    int32_t raw = mt6701_read_angle_raw();
                    s_base_mech_rad = raw_to_rad(raw);
                    s_haptic_filtered_velocity = 0.0f;
                    s_haptic_prev_detent_index_valid = false;
                    s_haptic_pulse_ticks_remaining = 0;
                    s_haptic_pulse_sign = 1.0f;
                    s_menu_prev_btn_a_pressed = false;
                    s_menu_prev_btn_b_pressed = false;
                    s_menu_prev_btn_c_pressed = false;
                    s_menu_prev_btn_d_pressed = false;
                    s_menu_btn_cooldown_until_iter = 0;
                    s_haptic_phase = HAPTIC_RUN;
                    s_haptic_start_us = esp_timer_get_time();
                    motor_sound_chime(); // the startup chime
                    // Kp/Kd/detent-count come from menu.c (live-adjustable, NVS-persisted) --
                    // not reset to compile-time defaults here anymore, so a saved setting from
                    // a previous session survives this arm just like calibration does.
                    ESP_LOGI(TAG, "ARMED: haptic detent demo, %lu detents (%.1f deg spacing), "
                                   "Kp=%.2f V/rad, Kd=%.3f V/(rad/s), running indefinitely -- turn the "
                                   "knob by hand, F4 opens the config menu (F3 back, F1 select)",
                             (unsigned long)menu_get_haptic_num_detents(),
                             360.0f / (float)menu_get_haptic_num_detents(),
                             (double)menu_get_haptic_kp(), (double)menu_get_haptic_kd());
                }
            } else if (s_polepair_test_mode) {
                s_pp_phase = PP_RAMP;
                s_pp_start_us = esp_timer_get_time();
                ESP_LOGI(TAG, "ARMED: pole-pair diagnostic, ramp to %.1f Hz electrical over %.1fs, "
                               "measure for %.1fs (configured pole_pairs=%d for comparison)",
                         PP_TEST_ELECTRICAL_HZ, ITERS_TO_SEC(PP_RAMP_ITERS), ITERS_TO_SEC(PP_MEASURE_ITERS),
                         MOTOR_POLE_PAIRS);
            } else {
            // Skip the calibration jerks if a sane calibration is already cached and a fresh
            // one wasn't explicitly requested -- the sensor/motor mounting doesn't change
            // between reboots, so a valid calibration remains valid indefinitely.
            bool have_cal = !force_recal && foc_calibration_load(&s_cal);
            if (!have_cal) {
                s_cal = foc_calibration_run();
                if (s_cal.valid) {
                    foc_calibration_save(&s_cal);
                }
            }

            if (!s_cal.valid) {
                ESP_LOGE(TAG, "calibration failed -- disabling, not attempting closed-loop control");
                motor_driver_enable(false);
            } else {
                int32_t raw = mt6701_read_angle_raw();
                s_base_mech_rad = raw_to_rad(raw);
                for (int i = 0; i < CL_NUM_TARGETS; i++) {
                    s_cl_target_offsets[i] = wrap_pi(2.0f * (float)M_PI * i / CL_NUM_TARGETS);
                }
                s_cl_phase = CL_HOLD;
                ESP_LOGI(TAG, "ARMED: closed-loop bench validation, %d targets x %.1fs each, "
                               "Kp=%.2f V/rad, Kd=%.3f V/(rad/s), diverge-abort at %.2f rad",
                         (int)CL_NUM_TARGETS, ITERS_TO_SEC(CL_TARGET_HOLD_ITERS), CL_KP, CL_KD,
                         CL_DIVERGE_ABORT_RAD);
            }
            }
        }
    }

    uint32_t iterations = 0;
    uint32_t prev_wake = 0;
    while (1) {
        // SYS INFO timing, in CPU cycles: the previous iteration's work ends here, and the
        // wake-up after the take starts this one. A notification count above 1 means ticks
        // arrived while the last iteration was still running -- those ticks are lost.
        uint32_t done = esp_cpu_get_cycle_count();
        uint32_t notified = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t wake = esp_cpu_get_cycle_count();
        if (iterations > 0) {
            sysmon_control_tick(done - prev_wake, wake - prev_wake, notified,
                                s_haptic_phase == HAPTIC_RUN ? s_applied_vq : 0.0f,
                                s_haptic_phase == HAPTIC_RUN ? s_applied_vd : 0.0f);
        }
        prev_wake = wake;

        iterations++;

        // Read-only sensor log, throttled to ~2Hz -- an ongoing sanity check that the
        // encoder is still readable independent of the closed-loop test below.
        // Logging temporarily disabled -- pure console noise during haptic-feel tuning.
        // Re-enable if debugging encoder read failures.
        if (false && iterations % MS_TO_ITERS(500) == 0) {
            int32_t raw = mt6701_read_angle_raw();
            ESP_LOGI(TAG, "MT6701 raw angle: %ld / 16383", (long)raw);
        }

        // Hard wall-clock safety net -- checked before the state machine, independent of
        // iteration counting (see comment above CL_HARD_TIMEOUT_US).
        if (s_cl_phase != CL_DONE && (esp_timer_get_time() - s_cl_start_us) > CL_HARD_TIMEOUT_US) {
            s_cl_phase = CL_DONE;
            motor_driver_enable(false);
            ESP_LOGE(TAG, "closed-loop test HARD TIMEOUT (%.1fs elapsed) -- forcing driver disabled",
                      (esp_timer_get_time() - s_cl_start_us) / 1000000.0f);
        }
        if (s_ol_phase != OL_TEST_DONE && (esp_timer_get_time() - s_ol_start_us) > OL_HARD_TIMEOUT_US) {
            s_ol_phase = OL_TEST_DONE;
            motor_driver_enable(false);
            ESP_LOGE(TAG, "open-loop diagnostic HARD TIMEOUT (%.1fs elapsed) -- forcing driver disabled",
                      (esp_timer_get_time() - s_ol_start_us) / 1000000.0f);
        }
        if (s_pp_phase != PP_DONE && (esp_timer_get_time() - s_pp_start_us) > PP_HARD_TIMEOUT_US) {
            s_pp_phase = PP_DONE;
            motor_driver_enable(false);
            ESP_LOGE(TAG, "pole-pair diagnostic HARD TIMEOUT (%.1fs elapsed) -- forcing driver disabled",
                      (esp_timer_get_time() - s_pp_start_us) / 1000000.0f);
        }
        // Haptic demo deliberately has NO time-based cutoff (unlike every other mode above) --
        // it's meant to be worn/used indefinitely, not a bounded scripted test. Still stops
        // immediately on a real fault via the sensor-read-failure and divergence aborts below.

        if (s_pp_phase != PP_DONE) {
            float electrical_hz;
            switch (s_pp_phase) {
                case PP_RAMP:
                    electrical_hz = PP_TEST_ELECTRICAL_HZ * ((float)s_pp_iter / PP_RAMP_ITERS);
                    if (++s_pp_iter >= PP_RAMP_ITERS) {
                        s_pp_iter = 0;
                        s_pp_phase = PP_MEASURE;
                        s_pp_accumulated_mech_rad = 0.0f;
                        s_pp_prev_mech_rad_valid = false;
                        ESP_LOGI(TAG, "pole-pair diagnostic: ramp complete, measuring for %.1fs",
                                 ITERS_TO_SEC(PP_MEASURE_ITERS));
                    }
                    break;
                case PP_MEASURE: {
                    electrical_hz = PP_TEST_ELECTRICAL_HZ;
                    int32_t raw = mt6701_read_angle_raw();
                    if (raw >= 0) {
                        // Accumulate the true (unwrapped) mechanical travel by summing shortest-
                        // path deltas between consecutive 1kHz samples -- the rotor can't move
                        // more than a small fraction of a revolution per tick, so wrap_pi's
                        // shortest-path assumption holds and correctly handles the 16383/0
                        // sensor wraparound without losing whole revolutions.
                        float mech_rad = raw_to_rad(raw);
                        if (s_pp_prev_mech_rad_valid) {
                            s_pp_accumulated_mech_rad += wrap_pi(mech_rad - s_pp_prev_mech_rad);
                        }
                        s_pp_prev_mech_rad = mech_rad;
                        s_pp_prev_mech_rad_valid = true;
                    }
                    if (++s_pp_iter >= PP_MEASURE_ITERS) {
                        float duration_s = ITERS_TO_SEC(PP_MEASURE_ITERS);
                        float mech_hz = fabsf(s_pp_accumulated_mech_rad) / (2.0f * (float)M_PI) / duration_s;
                        float computed_pole_pairs = mech_hz > 0.0001f ? PP_TEST_ELECTRICAL_HZ / mech_hz : 0.0f;
                        ESP_LOGI(TAG, "pole-pair diagnostic RESULT: %.3f rad mechanical over %.1fs "
                                       "(%.4f Hz mechanical) at %.2f Hz electrical -> computed "
                                       "pole_pairs=%.2f (currently configured: %d)",
                                 s_pp_accumulated_mech_rad, duration_s, mech_hz, PP_TEST_ELECTRICAL_HZ,
                                 computed_pole_pairs, MOTOR_POLE_PAIRS);
                        s_pp_iter = 0;
                        s_pp_phase = PP_RAMPDOWN;
                    }
                    break;
                }
                case PP_RAMPDOWN:
                default:
                    electrical_hz = PP_TEST_ELECTRICAL_HZ;
                    break;
            }

            s_pp_theta_e = wrap_pi(s_pp_theta_e + 2.0f * (float)M_PI * electrical_hz * (CONTROL_LOOP_PERIOD_US / 1000000.0f));

            float pp_vd = MOTOR_EFFECTIVE_ROTATING_VOLTAGE_LIMIT_V;
            if (s_pp_phase == PP_RAMPDOWN) {
                pp_vd = MOTOR_EFFECTIVE_ROTATING_VOLTAGE_LIMIT_V * (1.0f - (float)s_pp_iter / PP_RAMPDOWN_ITERS);
                if (++s_pp_iter >= PP_RAMPDOWN_ITERS) {
                    s_pp_phase = PP_DONE;
                    motor_driver_enable(false);
                    ESP_LOGI(TAG, "pole-pair diagnostic complete, driver disabled");
                }
            }

            if (s_pp_phase != PP_DONE) {
                foc_dq_t dq = { .d = pp_vd, .q = 0.0f }; // direct-axis trick, same as the open-loop
                                                          // diagnostic -- no feedback needed
                foc_ab_t ab = foc_inverse_park(dq, s_pp_theta_e);
                foc_abc_t abc = foc_inverse_clarke(ab);
                motor_driver_set_phase_voltages(abc.a, abc.b, abc.c);
            }
        }

        if (s_ol_phase != OL_TEST_DONE) {
            float current_rpm;
            switch (s_ol_phase) {
                case OL_RAMP:
                    current_rpm = OL_START_RPM + (OL_TARGET_RPM - OL_START_RPM) * ((float)s_ol_iter / OL_SPEED_RAMP_ITERS);
                    if (++s_ol_iter >= OL_SPEED_RAMP_ITERS) {
                        s_ol_iter = 0;
                        s_ol_phase = OL_HOLD;
                        ESP_LOGI(TAG, "open-loop diagnostic: target %.0f RPM reached, holding", OL_TARGET_RPM);
                    }
                    break;
                case OL_HOLD:
                    current_rpm = OL_TARGET_RPM;
                    if (++s_ol_iter >= OL_ROTATE_HOLD_ITERS) {
                        s_ol_iter = 0;
                        s_ol_phase = OL_RAMPDOWN;
                        ESP_LOGI(TAG, "open-loop diagnostic: hold complete, ramping down");
                    }
                    break;
                case OL_RAMPDOWN:
                default:
                    current_rpm = OL_TARGET_RPM;
                    break;
            }

            float electrical_hz = (current_rpm / 60.0f) * MOTOR_POLE_PAIRS;
            s_ol_theta_e = wrap_pi(s_ol_theta_e + 2.0f * (float)M_PI * electrical_hz * (CONTROL_LOOP_PERIOD_US / 1000000.0f));

            float vd = MOTOR_EFFECTIVE_ROTATING_VOLTAGE_LIMIT_V;
            if (s_ol_phase == OL_RAMPDOWN) {
                vd = MOTOR_EFFECTIVE_ROTATING_VOLTAGE_LIMIT_V * (1.0f - (float)s_ol_iter / OL_RAMPDOWN_ITERS_2);
                if (++s_ol_iter >= OL_RAMPDOWN_ITERS_2) {
                    s_ol_phase = OL_TEST_DONE;
                    motor_driver_enable(false);
                    ESP_LOGI(TAG, "open-loop diagnostic complete, driver disabled");
                }
            }

            if (s_ol_phase != OL_TEST_DONE) {
                foc_dq_t dq = { .d = vd, .q = 0.0f }; // direct-axis trick -- no feedback needed,
                                                       // forced commutation via a virtual
                                                       // rotating angle
                foc_ab_t ab = foc_inverse_park(dq, s_ol_theta_e);
                foc_abc_t abc = foc_inverse_clarke(ab);
                motor_driver_set_phase_voltages(abc.a, abc.b, abc.c);
            }

            if (iterations % MS_TO_ITERS(500) == 0 && s_ol_phase != OL_TEST_DONE) {
                ESP_LOGI(TAG, "open-loop: rpm=%.0f vd=%.3fV", current_rpm, vd);
            }
        }

        if (!s_open_loop_mode && s_cl_phase != CL_DONE) {
            int32_t raw = mt6701_read_angle_raw();
            if (raw < 0) {
                ESP_LOGE(TAG, "sensor read failed mid-test -- aborting closed-loop control");
                s_cl_phase = CL_DONE;
                motor_driver_enable(false);
            } else {
                float mech_rad = raw_to_rad(raw);
                float elec_rad = wrap_pi(s_cal.direction * mech_rad * MOTOR_POLE_PAIRS - s_cal.electrical_offset_rad);
                float target = wrap_pi(s_base_mech_rad + s_cl_target_offsets[s_cl_target_idx < CL_NUM_TARGETS ? s_cl_target_idx : 0]);
                float error = wrap_pi(target - mech_rad);

                float velocity = 0.0f;
                if (s_prev_mech_rad_valid) {
                    velocity = wrap_pi(mech_rad - s_prev_mech_rad) / (CONTROL_LOOP_PERIOD_US / 1000000.0f);
                }
                s_prev_mech_rad = mech_rad;
                s_prev_mech_rad_valid = true;

                if (s_cl_iter > CL_DIVERGE_CHECK_SETTLE_ITERS && fabsf(error) > CL_DIVERGE_ABORT_RAD) {
                    ESP_LOGE(TAG, "position error diverged (%.3f rad > %.2f rad abort threshold) -- "
                                   "likely a calibration sign error, not a real disturbance. Aborting.",
                             error, CL_DIVERGE_ABORT_RAD);
                    s_cl_phase = CL_DONE;
                    motor_driver_enable(false);
                } else {
                    float vq = CL_KP * error - CL_KD * velocity;
                    if (vq > MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq = MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                    if (vq < -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq = -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                    // elec_rad's slope wrt raw mech_rad is (direction*pole_pairs) -- positive Vq
                    // (at an accurate elec_rad) always drives elec_rad, hence raw mech_rad, in
                    // that same signed slope's direction. error/velocity above are computed
                    // directly in raw-mech_rad terms, so the PD effort must be re-signed by
                    // `direction` before injection, or a direction=-1 calibration (raw sensor
                    // counts running opposite the commutation convention -- confirmed correct
                    // via the calibration step's clean magnitude match) turns this into positive
                    // feedback: found on hardware immediately after the pole-pair fix (previous
                    // calibrations apparently always landed on direction=+1, which silently
                    // masked this).
                    vq *= s_cal.direction;
                    // Slew-limit relative to the last actually-applied vq (s_last_vq) --
                    // only meaningfully affects CL_HOLD, since CL_RAMP_DOWN below overwrites
                    // vq with its own already-gradual decay before it's ever applied.
                    if (vq > s_last_vq + CL_MAX_VQ_STEP_V) vq = s_last_vq + CL_MAX_VQ_STEP_V;
                    if (vq < s_last_vq - CL_MAX_VQ_STEP_V) vq = s_last_vq - CL_MAX_VQ_STEP_V;

                    switch (s_cl_phase) {
                        case CL_HOLD:
                            if (s_cl_iter == 0) {
                                s_ss_max_abs_error = 0.0f;
                                s_ss_sum_abs_error = 0.0f;
                                s_ss_count = 0;
                            }
                            if (s_cl_iter >= CL_TARGET_HOLD_ITERS - CL_STEADYSTATE_ITERS) {
                                float abs_err = fabsf(error);
                                if (abs_err > s_ss_max_abs_error) s_ss_max_abs_error = abs_err;
                                s_ss_sum_abs_error += abs_err;
                                s_ss_count++;
                            }
                            s_last_vq = vq;
                            if (++s_cl_iter >= CL_TARGET_HOLD_ITERS) {
                                ESP_LOGI(TAG, "target %lu/%d (%.3f rad) steady-state: max|err|=%.4f rad, "
                                               "avg|err|=%.4f rad",
                                         (unsigned long)(s_cl_target_idx + 1), (int)CL_NUM_TARGETS, target,
                                         s_ss_max_abs_error, s_ss_count ? s_ss_sum_abs_error / s_ss_count : 0.0f);
                                s_cl_iter = 0;
                                s_cl_target_idx++;
                                if (s_cl_target_idx >= CL_NUM_TARGETS) {
                                    s_cl_phase = CL_RAMP_DOWN;
                                    ESP_LOGI(TAG, "closed-loop test: all targets complete, ramping down");
                                }
                            }
                            break;
                        case CL_RAMP_DOWN:
                            vq = s_last_vq * (1.0f - (float)s_cl_iter / CL_RAMP_DOWN_ITERS);
                            if (++s_cl_iter >= CL_RAMP_DOWN_ITERS) {
                                s_cl_phase = CL_DONE;
                                motor_driver_enable(false);
                                ESP_LOGI(TAG, "closed-loop test complete, driver disabled");
                            }
                            break;
                        default:
                            break;
                    }

                    if (s_cl_phase != CL_DONE) {
                        foc_dq_t dq = { .d = 0.0f, .q = vq }; // quadrature axis -- real torque, unlike
                                                               // the open-loop test's direct-axis trick
                        foc_ab_t ab = foc_inverse_park(dq, elec_rad);
                        foc_abc_t abc = foc_inverse_clarke(ab);
                        motor_driver_set_phase_voltages(abc.a, abc.b, abc.c);
                    }

                    if (iterations % MS_TO_ITERS(500) == 0 && s_cl_phase != CL_DONE) {
                        ESP_LOGI(TAG, "target=%.3f error=%.4f rad, vq=%.3fV", target, error, vq);
                    }
                }
            }
        }

        if (s_haptic_phase != HAPTIC_DONE) {
            // SYS INFO's LOOP page: each part of the iteration timed in cycles.
            uint32_t sec_start = esp_cpu_get_cycle_count();
#define SECTION_DONE(sec) do { \
                uint32_t _now = esp_cpu_get_cycle_count(); \
                sysmon_control_section((sec), _now - sec_start); \
                sec_start = _now; \
            } while (0)
            if (s_haptic_phase == HAPTIC_RUN) {
                // Phase 8: F1-F4 (BTN_A-BTN_D) drive the real config menu (`menu.c`) --
                // fixed global roles, see DEVELOPMENT_PLAN.md Phase 8 and the Architecture
                // decisions log. F2 (Save) joined with the Pixel UI.
                // The companion's keys (EXT_CMD_INPUT) press the same as the real ones.
                const uint8_t vkeys = ext_virtual_keys();
                bool btn_a_pressed = (gpio_get_level(PIN_BTN_A) == 0) || (vkeys & UI_BTN_F1);
                bool btn_b_pressed = (gpio_get_level(PIN_BTN_B) == 0) || (vkeys & UI_BTN_F2);
                bool btn_c_pressed = (gpio_get_level(PIN_BTN_C) == 0) || (vkeys & UI_BTN_F3);
                bool btn_d_pressed = (gpio_get_level(PIN_BTN_D) == 0) || (vkeys & UI_BTN_F4);

                // Pixel UI: held state for the Main Screen keycaps (cheap atomic store, every
                // tick, same convention as ui_state_set_detent() below).
                ui_state_set_buttons((btn_a_pressed ? UI_BTN_F1 : 0) | (btn_b_pressed ? UI_BTN_F2 : 0)
                                     | (btn_c_pressed ? UI_BTN_F3 : 0) | (btn_d_pressed ? UI_BTN_F4 : 0));

                // While the attract animation runs, a press only wakes the screen (the display
                // sees the held mask above) -- it must not also open the menu.
                bool swallow = ui_state_get_screensaver();
                // An agent notification on screen (notify.h) answers to the keys while the menu
                // is closed. A key that goes down meanwhile is hidden from APP mode and the menu
                // until it's released, so the press that answers can't also play or skip a
                // track, even if it's still held once the notification is gone. Keys already
                // down when it appeared carry on, and the knob keeps its job throughout.
                // Only a finger on the knob approves: F1 (allow) counts from the real button
                // alone, never the companion's (EXT_CMD_INPUT, over USB or WiFi). Its F2-F4 only
                // deny or defer, so they still answer.
                const uint8_t raw_keys = ui_state_get_buttons();
                const uint8_t answer_keys = (raw_keys & ~UI_BTN_F1) | (gpio_get_level(PIN_BTN_A) == 0 ? UI_BTN_F1 : 0);
                bool notice = notify_active() && !menu_is_open();
                if (notice) notify_keys(answer_keys, esp_timer_get_time());
                s_notice_shown = notice; // the FORCE section's nudge reads it
                if (notice) s_notice_keys |= raw_keys & ~s_notice_raw_prev;
                s_notice_keys &= raw_keys;
                s_notice_raw_prev = raw_keys;

                // APP mode with the menu closed: F1-F4 are app controls (app_mode.c), and
                // long-press F4 is the way into the menu. Inside the menu they're menu keys
                // again. Both sides keep tracking the buttons every tick, so the press that
                // opens or closes the menu never also fires on the other side.
                bool app_active = menu_get_hid_type() == MENU_HID_APP && !menu_is_open();
                app_mode_update(app_active, esp_timer_get_time(), raw_keys & ~s_notice_keys, swallow);
                // HOME with the menu closed: F1-F3 are its keys (home.h), F4 still opens the menu.
                bool home_keys = menu_get_hid_type() == MENU_HID_HOME && !menu_is_open();
                // MIDI likewise (midi.h); F1 also reports its release (a hold picks a parameter).
                bool midi_keys = menu_get_hid_type() == MENU_HID_MIDI && !menu_is_open();
                if (midi_keys && !btn_a_pressed && s_menu_prev_btn_a_pressed) midi_input_key(UI_BTN_F1, false);

                if (motor_sound_cal_active()) {
                    // DEVICE -> SOUND CAL is running: F1-F3 answer it, F4 closes the menu,
                    // which stops it.
                    if (iterations >= s_menu_btn_cooldown_until_iter) {
                        uint8_t pressed = (btn_a_pressed && !s_menu_prev_btn_a_pressed ? UI_BTN_F1 : 0)
                                        | (btn_b_pressed && !s_menu_prev_btn_b_pressed ? UI_BTN_F2 : 0)
                                        | (btn_c_pressed && !s_menu_prev_btn_c_pressed ? UI_BTN_F3 : 0);
                        bool close = btn_d_pressed && !s_menu_prev_btn_d_pressed;
                        if (close) menu_input_toggle_open();
                        else motor_sound_cal_keys(pressed);
                        if (pressed || close) s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    }
                } else if (!app_active && !swallow && !notice && iterations >= s_menu_btn_cooldown_until_iter) {
                    if (btn_d_pressed && !s_menu_prev_btn_d_pressed) {
                        menu_input_toggle_open(); // F4
                        s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    } else if (btn_c_pressed && !s_menu_prev_btn_c_pressed) {
                        if (home_keys) home_input_key(UI_BTN_F3);
                        else if (midi_keys) midi_input_key(UI_BTN_F3, true);
                        else menu_input_back(); // F3
                        s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    } else if (btn_a_pressed && !s_menu_prev_btn_a_pressed) {
                        if (home_keys) home_input_key(UI_BTN_F1);
                        else if (midi_keys) midi_input_key(UI_BTN_F1, true);
                        else menu_input_select(); // F1
                        s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    } else if (btn_b_pressed && !s_menu_prev_btn_b_pressed) {
                        if (home_keys || midi_keys) {
                            if (home_keys) home_input_key(UI_BTN_F2);
                            else midi_input_key(UI_BTN_F2, true);
                            s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                        } else if (menu_current_screen() == MENU_SCREEN_HAPTIC) {
                            // Haptics: F2 saves on release; held 1.5 s it puts the shown
                            // profile back to factory instead.
                            s_f2_hold_since_iter = iterations;
                        } else {
                            menu_input_save(); // F2 -- an NVS commit when something changed, see menu.c
                            s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                        }
                    }
                }
                if (s_f2_hold_since_iter != 0) {
                    if (!btn_b_pressed) {
                        s_f2_hold_since_iter = 0;
                        menu_input_save();
                        s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    } else if (iterations - s_f2_hold_since_iter >= MS_TO_ITERS(1500)) {
                        s_f2_hold_since_iter = 0;
                        menu_input_reset_haptic();
                        s_menu_btn_cooldown_until_iter = iterations + MENU_BTN_COOLDOWN_ITERS;
                    }
                }
                s_menu_prev_btn_a_pressed = btn_a_pressed;
                s_menu_prev_btn_b_pressed = btn_b_pressed;
                s_menu_prev_btn_c_pressed = btn_c_pressed;
                s_menu_prev_btn_d_pressed = btn_d_pressed;

                // DEVICE -> RECALIBRATE: the same path as a first boot -- with no stored
                // calibration, the next boot aligns the motor again before haptics start.
                if (ext_restart_due()) { // the host asked (ext_proto.h EXT_CMD_REBOOT), e.g. to flash
                    ESP_LOGW(TAG, "restart requested by the host: motor off, restarting");
                    motor_driver_set_phase_voltages(0.0f, 0.0f, 0.0f);
                    motor_driver_enable(false);
                    esp_restart();
                }
                if (menu_take_recalibrate_request()) {
                    ESP_LOGW(TAG, "RECALIBRATE from the menu: motor off, calibration forgotten, restarting");
                    motor_driver_set_phase_voltages(0.0f, 0.0f, 0.0f);
                    motor_driver_enable(false);
                    foc_calibration_erase();
                    esp_restart();
                }
            }
            SECTION_DONE(SYSMON_SEC_INPUT);

            int32_t raw = mt6701_read_angle_raw();
            SECTION_DONE(SYSMON_SEC_SENSOR);
            if (raw < 0) {
                ESP_LOGE(TAG, "sensor read failed mid-test -- aborting haptic demo");
                s_haptic_phase = HAPTIC_DONE;
                motor_driver_enable(false);
            } else if (motor_sound_cal_active()) {
                // DEVICE -> SOUND CAL: no spring and no detents, only the tones and clicks
                // under test. The knob is free; turning it while one plays stops the
                // calibration (motor_sound.c).
                float mech_rad = raw_to_rad(raw);
                float elec_rad = wrap_pi(s_cal.direction * mech_rad * MOTOR_POLE_PAIRS - s_cal.electrical_offset_rad);
                motor_sound_cal_angle(mech_rad);
                float vd, vq;
                motor_sound_dq(0.0f, &vd, &vq);
                SECTION_DONE(SYSMON_SEC_FORCE);
                foc_dq_t dq = { .d = vd, .q = vq };
                foc_ab_t ab = foc_inverse_park(dq, elec_rad);
                foc_abc_t abc = foc_inverse_clarke(ab);
                motor_driver_set_phase_voltages(abc.a, abc.b, abc.c);
                s_applied_vq = vq;
                s_applied_vd = vd;
                SECTION_DONE(SYSMON_SEC_MOTOR);
                // Haptics start over from wherever the knob is when it ends: no step, no click.
                s_haptic_prev_detent_index_valid = false;
                s_prev_mech_rad_valid = false;
                s_haptic_filtered_velocity = 0.0f;
                s_haptic_pulse_ticks_remaining = 0;
            } else {
                float mech_rad = raw_to_rad(raw);
                float elec_rad = wrap_pi(s_cal.direction * mech_rad * MOTOR_POLE_PAIRS - s_cal.electrical_offset_rad);

                // Phase 8 step 3: live-tunable via the Haptic Configurator menu screen
                // (menu.c) instead of the retired button-combo scheme -- cached once per tick
                // (not re-read at each use site below) so detent_spacing/vq/wrapped_detent all
                // see a consistent set of values even if a menu edit lands mid-tick.
                // num_detents is clamped >=HAPTIC_NUM_DETENTS_MIN (3) by menu.c's
                // rotate_detents(), same bound this file always enforced -- never 0, which
                // would make detent_spacing below divide-by-zero.
                // Haptic profiles (haptic_params.h): the profile in force is the one the
                // Haptics screen shows while the menu is open (tune by feel), otherwise the
                // HID type's -- or, in APP mode, the live input's own. Its values are then
                // read once for this tick.
                bool app_on = menu_get_hid_type() == MENU_HID_APP && !menu_is_open();
                bool home_on = menu_get_hid_type() == MENU_HID_HOME && !menu_is_open();
                bool midi_on = menu_get_hid_type() == MENU_HID_MIDI && !menu_is_open();
                int haptic_profile = menu_haptic_profile();
                if (app_on) app_mode_haptics(&haptic_profile);
                else if (home_on) haptic_profile = home_haptic_profile(); // the list, or a value
                else if (midi_on) haptic_profile = midi_haptic_profile(); // the parameter's, or the list
                menu_haptic_set_active(haptic_profile);
                uint32_t num_detents = menu_get_haptic_num_detents();
                float kp = menu_get_haptic_kp();
                float kd = menu_get_haptic_kd();
                float shape = menu_get_haptic_shape();
                haptic_type_t haptic_type = menu_get_haptic_type();
                // A detent-count change (a key picking a different slot) re-bases the detent
                // grid: without this the index would jump and fire a spurious step.
                static uint32_t s_last_num_detents = 0;
                if (num_detents != s_last_num_detents) {
                    s_haptic_prev_detent_index_valid = false;
                    s_last_num_detents = num_detents;
                }


                // Nearest-grid-point selection: which of the N evenly-spaced detents is
                // nearest, and the (hysteresis-stabilized) error/rel/velocity relative to it.
                // Shared groundwork for all three profiles below -- it's also what drives the
                // detent-crossing edge used for HID scroll/menu-navigation dispatch further
                // down, which must keep working regardless of which restoring-force law
                // (Saw/Sine/Viscose, Phase 8 step 4) is currently selected.
                float detent_spacing = 2.0f * (float)M_PI / (float)num_detents;
                float rel = wrap_pi(mech_rad - s_base_mech_rad);

                // Hysteresis: stick with the previously committed detent until rel moves
                // past its midpoint by an extra margin, instead of always retargeting to
                // whichever is instantaneously nearest -- otherwise sensor noise alone
                // chatters the pick back and forth while sitting still at a midpoint.
                int32_t detent_index;
                if (s_haptic_prev_detent_index_valid) {
                    float committed_target_rel = (float)s_haptic_prev_detent_index * detent_spacing;
                    float dist_from_committed = wrap_pi(rel - committed_target_rel);
                    if (fabsf(dist_from_committed) > detent_spacing * (0.5f + HAPTIC_DETENT_HYSTERESIS_FRAC)) {
                        detent_index = (int32_t)roundf(rel / detent_spacing);
                    } else {
                        detent_index = s_haptic_prev_detent_index;
                    }
                } else {
                    detent_index = (int32_t)roundf(rel / detent_spacing);
                }
                // End stop: a crossing that would run a list off its end is refused.
                bool at_wall = false;
                if (s_haptic_prev_detent_index_valid && detent_index != s_haptic_prev_detent_index) {
                    float past = wrap_pi(rel - (float)s_haptic_prev_detent_index * detent_spacing);
                    int8_t dir = (past > 0 ? 1 : -1) * KNOB_DIRECTION; // same sense as the dispatch below
                    bool end = menu_is_open() ? menu_at_end(dir) : app_on ? app_mode_at_end(dir)
                             : home_on ? home_at_end(dir) : (midi_on && midi_at_end(dir));
                    if (end) {
                        detent_index = s_haptic_prev_detent_index;
                        at_wall = true;
                    }
                }
                static bool s_prev_at_wall = false; // LEDs flash once per new push into a wall
                if (at_wall && !s_prev_at_wall) ui_state_note_wall();
                s_prev_at_wall = at_wall;
                float target_rel = (float)detent_index * detent_spacing;
                float error = wrap_pi(target_rel - rel);

                float velocity = 0.0f;
                if (s_prev_mech_rad_valid) {
                    float delta = wrap_pi(mech_rad - s_prev_mech_rad);
                    velocity = delta / (CONTROL_LOOP_PERIOD_US / 1000000.0f);
                    app_mode_motion(delta * KNOB_DIRECTION, esp_timer_get_time()); // no-op outside APP mode
                    // Unwrapped knob travel for the display's 3D shape, in 1e-4 rad: an integer
                    // total plus a float remainder, so it never loses precision over many turns.
                    static int32_t s_knob_total = 0;
                    static float s_knob_frac = 0.0f;
                    s_knob_frac += delta * KNOB_DIRECTION * 10000.0f;
                    int32_t whole = (int32_t)s_knob_frac;
                    s_knob_frac -= (float)whole;
                    s_knob_total += whole;
                    ui_state_set_knob_angle(s_knob_total);
                }
                s_prev_mech_rad = mech_rad;
                s_prev_mech_rad_valid = true;
                s_haptic_filtered_velocity = HAPTIC_VELOCITY_FILTER_ALPHA * velocity
                                              + (1.0f - HAPTIC_VELOCITY_FILTER_ALPHA) * s_haptic_filtered_velocity;

                // error can never legitimately exceed half a detent's spacing (target is
                // always the NEAREST one) -- past a full spacing signals a real control bug
                // (e.g. the direction-sign class of bug found earlier), not normal operation.
                if (!at_wall && fabsf(error) > detent_spacing) { // pushing into a wall is allowed past this
                    ESP_LOGE(TAG, "haptic demo: error (%.3f rad) exceeds one full detent spacing "
                                   "(%.3f rad) -- likely a control bug, aborting.",
                             error, detent_spacing);
                    s_haptic_phase = HAPTIC_DONE;
                    motor_driver_enable(false);
                } else {
                    // Legacy-inspired coasting: above a fast hand-flick speed, apply zero
                    // torque and let the knob spin freely on momentum rather than fighting
                    // it -- only the slow/fine-adjustment regime gets the restoring spring.
                    // is_coasting is reused below by the click-pulse arming logic -- see
                    // that comment for why the pulse needs to know this too. Only meaningful
                    // for Saw/Sine, which have a real positional spring capable of injecting
                    // energy on a fast flick -- see the switch below for why Viscose ignores it.
                    bool is_coasting = fabsf(s_haptic_filtered_velocity) > HAPTIC_COAST_VELOCITY_RAD_S;
                    float vq;
                    // SINE into an end stop. Its force is a wave, not a spring: left alone it
                    // falls to zero at the midpoint and pushes ON towards the next step, and
                    // the wall's spring (below) then cut in at the switch point with a
                    // different force -- felt as an extra click. So towards an end, the wave is
                    // followed only up to its peak (a quarter step out); from there the push
                    // back holds that peak and stiffens, with no jump anywhere.
                    bool sine_wall = false;
                    float sine_peak_at = detent_spacing * 0.25f;
                    if (haptic_type == HAPTIC_TYPE_SINE && fabsf(error) > sine_peak_at) {
                        int8_t push_dir = (error < 0.0f ? 1 : -1) * KNOB_DIRECTION; // away from the committed step
                        sine_wall = at_wall || (menu_is_open() ? menu_at_end(push_dir)
                                                  : app_on ? app_mode_at_end(push_dir)
                                                  : home_on ? home_at_end(push_dir) : (midi_on && midi_at_end(push_dir)));
                    }
                    if (sine_wall) {
                        float push = kp * (1.0f + HAPTIC_WALL_GAIN * (fabsf(error) - sine_peak_at));
                        vq = (error >= 0.0f ? push : -push) - kd * s_haptic_filtered_velocity;
                    } else if (at_wall) {
                        // The normal spring back to the end detent, plus extra stiffness that
                        // starts at the switch point -- whatever the feel type, and not
                        // coast-gated: a flick into the end should still stop there.
                        float beyond = fabsf(error) - detent_spacing * (0.5f + HAPTIC_DETENT_HYSTERESIS_FRAC);
                        float extra = beyond > 0.0f ? HAPTIC_WALL_GAIN * kp * beyond : 0.0f;
                        vq = kp * error + (error >= 0.0f ? extra : -extra) - kd * s_haptic_filtered_velocity;
                    } else switch (haptic_type) {
                        case HAPTIC_TYPE_VISCOSE:
                            // Kp forced to 0 regardless of the menu's own Kp field -- pure
                            // velocity damping, no positional spring at all. Deliberately does
                            // NOT apply the coast-gate above: that gate exists to stop a
                            // positional spring from re-injecting energy during a fast flick,
                            // which a pure damper (always opposing velocity, never adding to
                            // it) can't do -- and cutting damping above a speed threshold would
                            // defeat viscose's whole point (feel like syrup at ANY speed, not
                            // just slow ones).
                            vq = -kd * s_haptic_filtered_velocity;
                            break;
                        case HAPTIC_TYPE_SINE:
                            // Continuous Vq = -Kp*sin(N*rel) - Kd*velocity -- a smooth "bump"
                            // through each detent instead of Saw's snap. Reuses the same Kp/Kd
                            // fields as Saw (no separate tunable exists in the menu for this).
                            // Historically rejected as the ONLY profile (steepest slope, hence
                            // strongest restoring push, sits at the detent CENTER -- opposite
                            // of a crisp click, and caused persistent oscillation sitting still
                            // there) but kept here as a deliberately different selectable feel.
                            vq = is_coasting ? 0.0f
                                             : (-kp * sinf((float)num_detents * rel) - kd * s_haptic_filtered_velocity);
                            break;
                        case HAPTIC_TYPE_SAW:
                        default: {
                            // SHAPE bends the straight line: the gain grows from (1 - shape)
                            // at the centre to 1 at the midpoint (u = 1), so the midpoint
                            // force is unchanged, the centre is softer and the rise comes
                            // later and steeper. shape = 0 is the plain kp * error. Held at 1
                            // past the midpoint (the hysteresis margin).
                            float u = fabsf(error) * (float)num_detents * (1.0f / (float)M_PI);
                            if (u > 1.0f) u = 1.0f;
                            float gain = 1.0f - shape + shape * u * u;
                            vq = is_coasting ? 0.0f : (kp * gain * error - kd * s_haptic_filtered_velocity);
                            break;
                        }
                    }
                    if (vq > MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq = MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                    if (vq < -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq = -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                    vq *= s_cal.direction; // see the bench-tour loop's comment above -- same fix applies here
                    if (vq > s_last_vq + HAPTIC_MAX_VQ_STEP_V) vq = s_last_vq + HAPTIC_MAX_VQ_STEP_V;
                    if (vq < s_last_vq - HAPTIC_MAX_VQ_STEP_V) vq = s_last_vq - HAPTIC_MAX_VQ_STEP_V;
                    s_last_vq = vq; // slew-limiter's anchor tracks the smooth background
                                     // only -- the transient pulse below is added AFTER
                                     // this, deliberately not slew-limited, and must not
                                     // corrupt next tick's slew reference

                    // Edge-detect an actual detent-index change (not a vague "threshold
                    // crossing" check) to trigger the click pulse exactly once per crossing.
                    // Captured AFTER vq is final (clamped, direction-corrected, slew-
                    // limited) so the pulse's sign can be tied to the SAME sense the
                    // background push already has at this instant -- without this, the
                    // pulse's fixed waveform phase would reinforce the snap on one turn
                    // direction and partially cancel against the background on the other,
                    // since the background's sign naturally flips depending on which way
                    // you cross into the next detent (expected/correct) while a
                    // direction-agnostic pulse wouldn't follow that flip.
                    if (s_haptic_prev_detent_index_valid && detent_index != s_haptic_prev_detent_index) {
                        // **Root cause of "spins fast on its own, still clicking" found on
                        // hardware**: this pulse-arming logic had no idea about the coast
                        // state above -- it fired a full, cap-level voltage kick on every
                        // single crossing regardless of whether the background torque had
                        // already gone to zero for coasting. During a fast flick that enters
                        // coast (meant to be a passive, torque-free spin on hand momentum),
                        // every crossing was still injecting an active push in the direction
                        // of travel -- actively adding energy to what should have been
                        // passive: more speed -> more crossings -> more kicks -> more speed,
                        // a real positive-feedback loop. Explains both symptoms reported
                        // together: reduced background resistance (that's coasting working
                        // as designed) *plus* continued active clicks accelerating the spin
                        // (that's this bug). Fixed: the pulse now only arms while NOT
                        // coasting, matching the background torque's own gate exactly --
                        // once genuinely coasting, no new clicks fire at all until velocity
                        // drops back into the normal (non-coast) range.
                        //
                        // Independently, only (re)arm if the previous pulse is past its
                        // IMPACT phase (the first ~4ms, deliberately over-driven to 6V and
                        // clipped to the safety cap) -- retriggering *during* that clipped
                        // window is what chained into a sustained near-max voltage before
                        // the coast-gate above existed (see DEVELOPMENT_PLAN.md Phase 2b).
                        // Guarding the whole ~24ms pulse (impact+tail) was tried first and
                        // was too coarse -- it also blocked retriggering during the much
                        // gentler ~1V tail, silently dropping most clicks during any
                        // moderately fast (but non-coasting) crossing cadence like normal
                        // menu browsing. The tail was never the dangerous part.
                        // Phase 8 step 4: the electrical click pulse is a deliberately
                        // over-driven (6V, clipped) voltage kick -- real BLDC noise, confirmed
                        // on hardware, not just a figure of speech. Saw's crisp snap wants that;
                        // Sine's whole point is a smooth continuous bump, so layering the same
                        // clipped kick on top of it fights its own character and was reported
                        // as an audible noise burst -- excluded here. Viscose excludes it too
                        // (no positional spring at all, nothing to "click" for). The audible
                        // click (motor_sound.h) is a separate burst -- still fires for
                        // Sine (a bump still benefits from a feedback cue) and for Viscose,
                        // on its virtual steps, at its profile's AMP (0 by default, 20% at
                        // most -- haptic_params.h). The HID scroll/menu-
                        // navigation dispatch further down is NOT gated by any of this -- that's
                        // the knob's actual input function, unrelated to haptic feel.
                        bool electrical_pulse_enabled = (haptic_type == HAPTIC_TYPE_SAW);
                        bool audio_click_enabled = true; // VISCOSE too: its profile caps AMP at 20%, 0 by default
                        if (electrical_pulse_enabled) {
                            uint32_t ticks_since_arm = HAPTIC_PULSE_DURATION_ITERS - s_haptic_pulse_ticks_remaining;
                            bool past_impact_phase = (s_haptic_pulse_ticks_remaining == 0)
                                                   || (ticks_since_arm >= HAPTIC_PULSE_IMPACT_DURATION_ITERS);
                            if (!is_coasting && past_impact_phase) {
                                s_haptic_pulse_ticks_remaining = HAPTIC_PULSE_DURATION_ITERS;
                                s_haptic_pulse_sign = (vq >= 0.0f) ? 1.0f : -1.0f;
                            }
                        }
                        if (audio_click_enabled) {
                            // The same detent-index edge that triggers the electrical click
                            // pulse above also triggers the audible one, a burst on the motor
                            // (motor_sound.h). Fires menu open or not -- same physical click
                            // either way, only what the crossing *means* (below) changes.
                            motor_sound_click(menu_get_haptic_pitch(), menu_get_click_amplitude(), menu_get_click_shape());
                            ui_state_note_click();
                        }

                        // Direction comes from the filtered rotation velocity's sign at
                        // this instant, NOT detent_index's own increasing/decreasing
                        // value -- detent_index is derived from a wrap_pi'd angle, so its
                        // raw integer value jumps once per full revolution at the +-pi
                        // wrap boundary; velocity has no such discontinuity.
                        int8_t dir = (s_haptic_filtered_velocity >= 0.0f ? 1 : -1) * KNOB_DIRECTION;
                        dispatch_turn(dir, app_on);
                    }
                    s_haptic_prev_detent_index = detent_index;
                    s_haptic_prev_detent_index_valid = true;

                    // Phase 4: live "current detent" readout for the default screen --
                    // wrapped to 0..(num_detents-1) since the raw detent_index is an
                    // unbounded counter from wherever the device booted, not a meaningful
                    // position on its own. Updated every tick (cheap atomic store), not
                    // just on the edge above, so it's never stale.
                    // The companion's turns (EXT_CMD_INPUT): a detent at a time at a hand's pace,
                    // through the same door as the knob's own -- no click, the knob didn't move.
                    static uint32_t s_vturn_wait = 0;
                    if (s_vturn_wait) {
                        s_vturn_wait--;
                    } else {
                        int8_t vdir = ext_take_virtual_turn();
                        if (vdir) {
                            s_vturn_wait = MS_TO_ITERS(15);
                            if (menu_is_open() ? menu_at_end(vdir) : (app_on && app_mode_at_end(vdir))) ui_state_note_wall();
                            else dispatch_turn(vdir, app_on);
                        }
                    }

                    int32_t wrapped_detent = ((detent_index % (int32_t)num_detents)
                                              + (int32_t)num_detents) % (int32_t)num_detents;
                    ui_state_set_detent(wrapped_detent);

                    // Click transient: a two-stage impact+tail added on top of the smooth
                    // background Vq, triggered on the detent-index edge above. Deliberately
                    // NOT slew-limited (see HAPTIC_PULSE_* comment) -- this is what actually
                    // makes the transition fast enough to be audible, rather than smeared
                    // over ~13ms like the background alone would be.
                    float vq_out = vq;
                    if (s_haptic_pulse_ticks_remaining > 0) {
                        uint32_t ticks_elapsed = HAPTIC_PULSE_DURATION_ITERS - s_haptic_pulse_ticks_remaining;
                        float dt_s = CONTROL_LOOP_PERIOD_US / 1000000.0f;
                        float transient;
                        if (ticks_elapsed < HAPTIC_PULSE_IMPACT_DURATION_ITERS) {
                            // Impact: constant amplitude for the full short burst -- no
                            // envelope decay here, that's what keeps the attack sharp.
                            float elapsed_s = ticks_elapsed * dt_s;
                            transient = HAPTIC_PULSE_IMPACT_AMPLITUDE_V
                                       * sinf(2.0f * (float)M_PI * HAPTIC_PULSE_IMPACT_FREQ_HZ * elapsed_s);
                        } else {
                            // Tail: decaying ring-down, same shape as the original single-
                            // stage pulse, just lower amplitude/frequency and starting from
                            // wherever the impact left off.
                            uint32_t tail_ticks_elapsed = ticks_elapsed - HAPTIC_PULSE_IMPACT_DURATION_ITERS;
                            float progress = 1.0f - (float)tail_ticks_elapsed / HAPTIC_PULSE_TAIL_DURATION_ITERS; // 1->0
                            float elapsed_s = tail_ticks_elapsed * dt_s;
                            transient = HAPTIC_PULSE_TAIL_AMPLITUDE_V
                                       * sinf(2.0f * (float)M_PI * HAPTIC_PULSE_TAIL_FREQ_HZ * elapsed_s)
                                       * progress;
                        }
                        vq_out += s_haptic_pulse_sign * transient;
                        if (vq_out > MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq_out = MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                        if (vq_out < -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq_out = -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                        s_haptic_pulse_ticks_remaining--;
                    }
                    // An agent waiting for an answer (notify.h): a gentle double tap now and
                    // then -- like the click, on top of the spring and not slew-limited.
                    float nudge = notify_nudge_vq(esp_timer_get_time(), s_notice_shown);
                    if (nudge != 0.0f) {
                        vq_out += nudge;
                        if (vq_out > MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq_out = MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                        if (vq_out < -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V) vq_out = -MOTOR_EFFECTIVE_STATIC_VOLTAGE_LIMIT_V;
                    }

                    SECTION_DONE(SYSMON_SEC_FORCE);
                    if (s_haptic_phase != HAPTIC_DONE) {
                        // The motor's sound, on the d or the q axis, in the voltage the haptics leave.
                        float vd_out;
                        motor_sound_dq(vq_out, &vd_out, &vq_out);
                        foc_dq_t dq = { .d = vd_out, .q = vq_out };
                        foc_ab_t ab = foc_inverse_park(dq, elec_rad);
                        foc_abc_t abc = foc_inverse_clarke(ab);
                        motor_driver_set_phase_voltages(abc.a, abc.b, abc.c);
                        s_applied_vq = vq_out;
                        s_applied_vd = vd_out;
                    }
                    SECTION_DONE(SYSMON_SEC_MOTOR);
                    // No periodic logging here (there was a 5 Hz "haptic:" line and a 1 Hz
                    // "control loop alive"): SYS INFO showed each costing a ~13 ms stall, as
                    // the log write waits on the USB console, and ~130 missed ticks with it.
                    // sysmon's 5 s `sys` line is the heartbeat now.
                }
            }
#undef SECTION_DONE
        }
    }
}

void control_task_start(void) {
    xTaskCreatePinnedToCore(control_task_fn, "control", 4096, NULL, PRIO_CONTROL, &s_task_handle, CORE_CONTROL);

    // 1 MHz count, an alarm every CONTROL_LOOP_PERIOD_US, reloading. The interrupt lands on
    // the core that registers the callback: app_main's, Core 0 -- the control task's own.
    const gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_cfg, &s_pacing_timer));
    const gptimer_event_callbacks_t cbs = {.on_alarm = pacing_timer_cb};
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_pacing_timer, &cbs, NULL));
    const gptimer_alarm_config_t alarm = {
        .alarm_count = CONTROL_LOOP_PERIOD_US,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_pacing_timer, &alarm));
    ESP_ERROR_CHECK(gptimer_enable(s_pacing_timer));
    ESP_ERROR_CHECK(gptimer_start(s_pacing_timer));
}
