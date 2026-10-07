#pragma once

// Phase 8 step 3: haptic detent parameters, live-tunable via the Haptic Configurator menu
// screen (menu.c) and consumed directly by the real-time haptic control loop
// (control_task.c). Pulled into their own shared header instead of living only as
// control_task.c's private #defines (which is where they lived through step 2) because
// menu.c's rotate_*()/default-value code needs the EXACT same bounds -- duplicating them as
// separate literals in two files is exactly the kind of drift this project has already been
// bitten by before (see MS_TO_ITERS()'s history in DEVELOPMENT_PLAN.md), and would be
// actively unsafe here specifically: menu.c's detent-count field used to be a cosmetic
// step-1 placeholder (clamped 0-120, wired to nothing real). Now that it directly drives
// `2*pi/num_detents` in control_task.c's real-time loop, an unclamped or wrongly-bounded
// value -- 0 above all -- is a live division-by-zero/NaN hazard on the actual motor output,
// not just a UI quirk.

#define HAPTIC_NUM_DETENTS_DEFAULT 12u // 30deg spacing
#define HAPTIC_NUM_DETENTS_MIN 3u
#define HAPTIC_NUM_DETENTS_MAX 36u

#define HAPTIC_KP_DEFAULT 6.0f // V/rad -- "sharpness". Likely saturates against the voltage
                                // cap over much of a detent's travel (that's bounded by
                                // MOTOR_MAX_CURRENT_STATIC_A, a separate knob, not this gain)
#define HAPTIC_KP_MIN 0.0f      // 0 = no detents at all, pure Kd -- see HAPTIC_KD_MAX's
                                // "viscous fluid" note
#define HAPTIC_KP_MAX 20.0f

#define HAPTIC_KD_DEFAULT 0.01f // V per (rad/s)
#define HAPTIC_KD_MIN 0.0f
#define HAPTIC_KD_MAX 0.15f // HAPTIC_KP_MIN + Kd near this ceiling gives a distinct "viscous
                             // fluid" knob feel -- pure velocity damping, no positional spring

// SHAPE (percent): bends Saw's straight force line. 0 = straight, as before. Higher values
// soften the pull near a step's centre and move the rise towards the midpoint between two
// steps, where it gets steeper; the force at the midpoint itself stays the same
// (control_task.c). Capped below 100 so the centre always keeps some spring.
#define HAPTIC_SHAPE_DEFAULT 0
#define HAPTIC_SHAPE_MIN 0
#define HAPTIC_SHAPE_MAX 90
#define HAPTIC_SHAPE_STEP 5

// Phase 8 step 4: the three selectable haptic profiles (Haptic Configurator's "Haptic Type"
// field, menu.c) -- shared with control_task.c, which is the one that actually branches on
// this to pick a restoring-force law each tick, and with config_store.c's load-time sanity
// check (previously a separate hand-synced HAPTIC_TYPE_COUNT define there -- now just this
// enum's own count, one less place for the two to drift apart).
typedef enum {
    HAPTIC_TYPE_SAW = 0, // nearest-grid-point linear snap -- the original/default profile
    HAPTIC_TYPE_SINE,    // continuous Vq = -Kp*sin(num_detents*rel) -- a smooth "bump" instead
                         // of a snap; historically rejected as the ONLY profile (steepest
                         // slope at the detent center caused persistent oscillation there,
                         // flattest at the boundary -- backwards for a crisp click) but kept
                         // here as a deliberately different, selectable feel, not a default
    HAPTIC_TYPE_VISCOSE, // Kp forced to 0 regardless of the menu's own Kp field -- pure
                         // velocity damping, no positional spring, no clicks
    HAPTIC_TYPE_COUNT
} haptic_type_t;

// --- Haptic profiles ---
// The Haptics STEPS choices are complete, reusable feels: a spacing plus its own FEEL, SNAP,
// DAMP, SHAPE, AMP and PITCH. Modes and app-profile inputs pick one by id. The user tunes a
// profile inside the limits given here, per feel, so a spacing can't be tuned into
// instability; values are kept per feel, so switching FEEL shows that feel's own (saved or
// factory) values. menu.c holds the live copies and NVS the saved ones.
//
// Factory values: the user's, tuned on hardware (2026-10-02), for each profile's factory
// feel. A stepped profile's other feel starts from the same numbers until it is tuned too.
// The LIMITS are still the old global ranges (HAPTIC_LIM_TODO) -- to be narrowed per profile
// and feel.
typedef enum {
    HAPTIC_PROFILE_WIDE = 0,
    HAPTIC_PROFILE_COARSE,
    HAPTIC_PROFILE_MEDIUM,
    HAPTIC_PROFILE_FINE,
    HAPTIC_PROFILE_SMOOTH, // VISCOSE only: drags and free scrolling
    HAPTIC_PROFILE_COUNT
} haptic_profile_id_t;
#define HAPTIC_PROFILE_STEPPED_COUNT 4 // WIDE..FINE, in spacing order

typedef struct {
    float kp;      // SNAP
    float kd;      // DAMP
    int32_t shape; // percent, SAW only
    int32_t amp;   // click amplitude, percent
    float pitch;   // click pitch multiplier
} haptic_tune_t;

// AMP: percent, in steps of 5; each profile and feel has its own ceiling (amp_max).
#define HAPTIC_CLICK_AMP_MIN 0
#define HAPTIC_CLICK_AMP_STEP 5

typedef struct {
    float kp_min, kp_max;
    float kd_min, kd_max;
    int32_t amp_max;
    float pitch_min, pitch_max;
} haptic_limits_t;

typedef struct {
    const char *name;
    uint8_t detents; // per turn. SMOOTH has no felt steps; these are where step events fire
    uint8_t feels;   // allowed feels, 1 << haptic_type_t
    uint8_t feel;    // factory feel
    haptic_tune_t tune[HAPTIC_TYPE_COUNT];  // factory values, per feel: SAW, SINE, VISCOSE
    haptic_limits_t lim[HAPTIC_TYPE_COUNT]; // safe range, per feel
} haptic_profile_t;

// WIDE..FINE offer SAW and SINE; VISCOSE belongs to SMOOTH alone.
#define HAPTIC_FEELS_STEPPED ((1u << HAPTIC_TYPE_SAW) | (1u << HAPTIC_TYPE_SINE))
#define HAPTIC_FEELS_SMOOTH (1u << HAPTIC_TYPE_VISCOSE)
// VISCOSE: no SNAP, clicks never louder than 20%, pitch 1-2x.
#define HAPTIC_LIM_VISCOSE {0.0f, 0.0f, HAPTIC_KD_MIN, HAPTIC_KD_MAX, 20, 1.0f, 2.0f}
// Placeholder limits for the stepped feels.
#define HAPTIC_LIM_TODO {HAPTIC_KP_MIN, HAPTIC_KP_MAX, HAPTIC_KD_MIN, HAPTIC_KD_MAX, 100, 0.5f, 2.0f}
// A feel a profile doesn't offer: never used, present so the tables stay indexed by feel.
#define HAPTIC_TUNE_NONE {0.0f, 0.0f, 0, 0, 1.0f}

__attribute__((unused)) static const haptic_profile_t HAPTIC_PROFILES[HAPTIC_PROFILE_COUNT] = {
    // name, detents, feels, factory feel,
    //   tune {SNAP, DAMP, SHAPE %, AMP %, PITCH}: SAW, SINE, VISCOSE
    //   limits: SAW, SINE, VISCOSE
    {"WIDE", 8, HAPTIC_FEELS_STEPPED, HAPTIC_TYPE_SAW,
     {{6.00f, 0.005f, 25, 100, 0.85f}, {6.00f, 0.005f, 0, 100, 0.85f}, HAPTIC_TUNE_NONE},
     {HAPTIC_LIM_TODO, HAPTIC_LIM_TODO, HAPTIC_LIM_VISCOSE}},
    {"COARSE", 12, HAPTIC_FEELS_STEPPED, HAPTIC_TYPE_SINE,
     {{2.00f, 0.035f, 0, 100, 0.90f}, {2.00f, 0.035f, 0, 100, 0.90f}, HAPTIC_TUNE_NONE},
     {HAPTIC_LIM_TODO, HAPTIC_LIM_TODO, HAPTIC_LIM_VISCOSE}},
    {"MEDIUM", 24, HAPTIC_FEELS_STEPPED, HAPTIC_TYPE_SAW,
     {{4.00f, 0.115f, 55, 90, 1.20f}, {4.00f, 0.115f, 0, 90, 1.20f}, HAPTIC_TUNE_NONE},
     {HAPTIC_LIM_TODO, HAPTIC_LIM_TODO, HAPTIC_LIM_VISCOSE}},
    {"FINE", 36, HAPTIC_FEELS_STEPPED, HAPTIC_TYPE_SAW,
     {{1.50f, 0.150f, 80, 70, 1.95f}, {1.50f, 0.150f, 0, 70, 1.95f}, HAPTIC_TUNE_NONE},
     {HAPTIC_LIM_TODO, HAPTIC_LIM_TODO, HAPTIC_LIM_VISCOSE}},
    {"SMOOTH", 24, HAPTIC_FEELS_SMOOTH, HAPTIC_TYPE_VISCOSE,
     {HAPTIC_TUNE_NONE, HAPTIC_TUNE_NONE, {0.0f, 0.150f, 0, 15, 1.85f}},
     {HAPTIC_LIM_TODO, HAPTIC_LIM_TODO, HAPTIC_LIM_VISCOSE}},
};

// The stepped profile whose spacing is nearest to `detents` per turn.
static inline int haptic_profile_nearest(unsigned detents) {
    int best = 0;
    unsigned best_d = ~0u;
    for (int i = 0; i < HAPTIC_PROFILE_STEPPED_COUNT; i++) {
        unsigned p = HAPTIC_PROFILES[i].detents;
        unsigned dist = p > detents ? p - detents : detents - p;
        if (dist < best_d) {
            best_d = dist;
            best = i;
        }
    }
    return best;
}
// What an app-profile input written as (feel, detents) uses: VISCOSE -> SMOOTH, a count ->
// the nearest stepped profile, no count -> -1 (the mode's own profile).
static inline int haptic_profile_for(haptic_type_t feel, unsigned detents) {
    if (feel == HAPTIC_TYPE_VISCOSE) return HAPTIC_PROFILE_SMOOTH;
    return detents ? haptic_profile_nearest(detents) : -1;
}
