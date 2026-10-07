#include "notify.h"
#include "motor_sound.h"
#include "tasks_common.h"
#include "ui_state.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

// The queue: written by the host side (Core 1) and by a decision (Core 0), so it lives under
// a spinlock, held only for a few small copies. The control loop never takes it per tick: the
// item on show is mirrored in one atomic word (s_top) that it reads instead.
static notify_item_t s_items[NOTIFY_MAX];
static int s_count = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define TOP_VALID (1u << 31)
#define TOP_NUDGE (1u << 30)
static _Atomic uint32_t s_top = 0;     // 0 = none, else TOP_VALID | TOP_NUDGE? | kind << 16 | id
static _Atomic uint32_t s_version = 0;
static _Atomic uint32_t s_hold_pm = 0; // F1 hold progress, per mille

// The nudge: two soft bursts, a raised-cosine envelope each, well under the detent click's
// tail (1 V at 70 Hz) so it reads as a polite tap rather than a click.
#define NUDGE_V 0.6f
#define NUDGE_HZ 120.0f
#define NUDGE_BURST_US 35000
#define NUDGE_GAP_US 105000
static int64_t s_nudge_at = 0; // when the current pattern started (0 = none since shown)

// Decisions on their way to the host: control task -> usb task, single producer and consumer.
#define EVENT_RING 8
static uint32_t s_events[EVENT_RING]; // id << 8 | decision
static _Atomic uint32_t s_ev_head = 0, s_ev_tail = 0;

// Keys, as the control task sees them (Core 0 only).
#define DEBOUNCE_US 15000
#define LOCKOUT_US 250000 // after a decision: one bouncy or double press can't answer the next item
static uint8_t s_raw_prev, s_stable, s_prev_live, s_ignore;
static int64_t s_changed_at[4];
static uint32_t s_shown; // the s_top value the key state belongs to
static int64_t s_f1_since, s_locked_until;

static void publish_top_locked(void) {
    uint32_t top = 0;
    if (s_count > 0) {
        top = TOP_VALID | (uint32_t)s_items[0].kind << 16 | s_items[0].id;
        if (s_items[0].flags & NOTIFY_FLAG_NUDGE) top |= TOP_NUDGE;
    }
    atomic_store(&s_top, top);
    atomic_fetch_add(&s_version, 1);
}

static void remove_at_locked(int i) {
    memmove(&s_items[i], &s_items[i + 1], (size_t)(s_count - i - 1) * sizeof(s_items[0]));
    s_count--;
}

void notify_post(const notify_item_t *it) {
    portENTER_CRITICAL(&s_mux);
    int i = 0;
    while (i < s_count && s_items[i].id != it->id) i++;
    if (i == s_count && s_count == NOTIFY_MAX) {
        // Full: an attention item makes room before an approval is refused (the host then
        // falls back to the app's own prompt).
        int victim = -1;
        for (int k = 0; k < s_count && victim < 0; k++) {
            if (s_items[k].kind == NOTIFY_INFO) victim = k;
        }
        if (victim < 0) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
        remove_at_locked(victim);
        i = s_count;
    }
    s_items[i] = *it;
    s_items[i].title[NOTIFY_TITLE_MAX] = '\0';
    s_items[i].body[NOTIFY_BODY_MAX] = '\0';
    if (i == s_count) s_count++;
    publish_top_locked();
    portEXIT_CRITICAL(&s_mux);
}

void notify_clear(uint16_t id) {
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id == id) {
            remove_at_locked(i);
            publish_top_locked();
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

void notify_clear_all(void) {
    portENTER_CRITICAL(&s_mux);
    s_count = 0;
    publish_top_locked();
    portEXIT_CRITICAL(&s_mux);
}

bool notify_take_event(uint16_t *id, uint8_t *decision) {
    uint32_t tail = atomic_load(&s_ev_tail);
    if (tail == atomic_load(&s_ev_head)) return false;
    uint32_t e = s_events[tail % EVENT_RING];
    atomic_store(&s_ev_tail, tail + 1);
    *id = (uint16_t)(e >> 8);
    *decision = (uint8_t)(e & 0xFF);
    return true;
}

bool CONTROL_HOT notify_active(void) {
    return atomic_load_explicit(&s_top, memory_order_relaxed) != 0;
}

static void CONTROL_HOT decide(uint32_t top, uint8_t decision, uint8_t held, int64_t now_us) {
    uint16_t id = (uint16_t)(top & 0xFFFF);
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].id == id) {
            remove_at_locked(i);
            publish_top_locked();
            break;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    uint32_t head = atomic_load(&s_ev_head);
    if (head - atomic_load(&s_ev_tail) < EVENT_RING) {
        s_events[head % EVENT_RING] = (uint32_t)id << 8 | decision;
        atomic_store(&s_ev_head, head + 1);
    }
    // The answer is heard: allowed, the coin; denied, the low note. If another item is waiting
    // its own tune takes over on the next tick.
    if (decision == NOTIFY_ALLOW) motor_sound_jingle(MOTOR_SOUND_JINGLE_ALLOW);
    else if (decision == NOTIFY_DENY) motor_sound_jingle(MOTOR_SOUND_JINGLE_CANCEL);
    s_ignore = held; // the answering press says nothing about the next item
    s_f1_since = 0;
    s_locked_until = now_us + LOCKOUT_US;
    atomic_store(&s_hold_pm, 0);
}

void CONTROL_HOT notify_keys(uint8_t raw, int64_t now_us) {
    // Debounce: a key's level counts once it has been steady for DEBOUNCE_US.
    for (int b = 0; b < 4; b++) {
        uint8_t m = 1u << b;
        if ((raw ^ s_raw_prev) & m) s_changed_at[b] = now_us;
        if (now_us - s_changed_at[b] >= DEBOUNCE_US) s_stable = (s_stable & ~m) | (raw & m);
    }
    s_raw_prev = raw;

    uint32_t top = atomic_load(&s_top);
    if (top != s_shown) { // a new item on show: keys already down are about something else
        s_shown = top;
        s_ignore = s_stable;
        s_f1_since = 0;
        atomic_store(&s_hold_pm, 0);
    }
    s_ignore &= s_stable; // a released key counts again
    uint8_t live = s_stable & ~s_ignore;
    uint8_t pressed = live & ~s_prev_live;
    s_prev_live = live;
    if (top == 0 || now_us < s_locked_until) {
        s_f1_since = 0;
        atomic_store(&s_hold_pm, 0);
        return;
    }

    if (((top >> 16) & 0xFF) == NOTIFY_INFO) {
        if (pressed) decide(top, NOTIFY_DISMISS, s_stable, now_us);
        return;
    }
    if (pressed & UI_BTN_F3) {
        decide(top, NOTIFY_DENY, s_stable, now_us);
        return;
    }
    if (pressed & (UI_BTN_F2 | UI_BTN_F4)) {
        decide(top, NOTIFY_LATER, s_stable, now_us);
        return;
    }
    if (live & UI_BTN_F1) {
        if (s_f1_since == 0) s_f1_since = now_us;
        int64_t pm = (now_us - s_f1_since) / NOTIFY_HOLD_MS;
        if (pm >= 1000) {
            decide(top, NOTIFY_ALLOW, s_stable, now_us);
            return;
        }
        atomic_store(&s_hold_pm, (uint32_t)pm);
    } else if (s_f1_since != 0) {
        s_f1_since = 0; // let go too early: nothing happens
        atomic_store(&s_hold_pm, 0);
    }
}

float CONTROL_HOT notify_nudge_vq(int64_t now_us, bool shown) {
    uint32_t top = atomic_load_explicit(&s_top, memory_order_relaxed);
    // A tune when an item comes on show, whether it taps or not (only items that need input
    // do): once per item, in any mode, as soon as the menu is closed. An approval (ASK) gets
    // the three rising notes, anything else the two soft ones. It hung on the tap at first,
    // and most notifications never tapped.
    static uint32_t s_chimed = 0; // the s_top the tune was last played for
    if (!top) s_chimed = 0;
    else if (shown && top != s_chimed) {
        s_chimed = top;
        motor_sound_jingle(((top >> 16) & 0xFF) == NOTIFY_ASK ? MOTOR_SOUND_JINGLE_CHIME : MOTOR_SOUND_JINGLE_AGENT);
    }
    if (!shown || !(top & TOP_NUDGE) || s_f1_since != 0) {
        s_nudge_at = 0; // the next time an item needs input, it taps at once
        return 0.0f;
    }
    if (s_nudge_at == 0 || now_us - s_nudge_at >= (int64_t)NOTIFY_NUDGE_PERIOD_MS * 1000) s_nudge_at = now_us;
    int64_t t = now_us - s_nudge_at;
    if (t >= NUDGE_BURST_US + NUDGE_GAP_US) t -= NUDGE_BURST_US + NUDGE_GAP_US; // the second tap
    else if (t >= NUDGE_BURST_US) return 0.0f;
    if (t >= NUDGE_BURST_US) return 0.0f;
    float u = (float)t / NUDGE_BURST_US;                    // 0..1 through the burst
    float env = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * u); // soft in, soft out
    return NUDGE_V * env * sinf(2.0f * (float)M_PI * NUDGE_HZ * (float)t * 1e-6f);
}

bool notify_peek(notify_item_t *out, int *count, float *hold) {
    portENTER_CRITICAL(&s_mux);
    int n = s_count;
    if (n > 0) *out = s_items[0];
    portEXIT_CRITICAL(&s_mux);
    *count = n;
    *hold = atomic_load(&s_hold_pm) / 1000.0f;
    return n > 0;
}

int notify_colors(uint32_t *out, int max) {
    portENTER_CRITICAL(&s_mux);
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) out[i] = notify_color(&s_items[i]);
    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t notify_version(void) { return atomic_load(&s_version); }

uint32_t notify_color(const notify_item_t *it) {
    if (it->color) return it->color & 0xFFFFFFu;
    switch (it->source) {
        case NOTIFY_SRC_CLAUDE: return 0xE8825Fu; // Claude's coral
        case NOTIFY_SRC_CODEX: return 0x4F9DFFu;
        case NOTIFY_SRC_CURSOR: return 0xB98CFFu;
        default: return 0xFFFFFFu;
    }
}

const char *notify_source_name(uint8_t source) {
    switch (source) {
        case NOTIFY_SRC_CLAUDE: return "CLAUDE CODE";
        case NOTIFY_SRC_CODEX: return "CODEX";
        case NOTIFY_SRC_CURSOR: return "CURSOR";
        default: return "AGENT";
    }
}
