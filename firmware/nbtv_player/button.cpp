#include "button.h"
#include "nbtv_config.h"

#include <Arduino.h>

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_MS     = 800;
static const uint32_t DOUBLE_MS   = 350;

static bool     s_last = false;      // debounced pressed state
static uint32_t s_edge = 0;          // time of last state change
static uint32_t s_press_start = 0;
static bool     s_long_sent = false;
static uint32_t s_pending_release = 0;  // time of a single release awaiting double
static bool     s_await_double = false;

void button_init() {
    pinMode(PIN_BUTTON, INPUT);
}

bool button_down_now() {
    return digitalRead(PIN_BUTTON) == LOW;  // active low
}

ButtonEvent button_poll() {
    uint32_t now = millis();
    bool raw = button_down_now();

    if (raw != s_last && (now - s_edge) > DEBOUNCE_MS) {
        s_edge = now;
        s_last = raw;
        if (raw) {                       // press
            s_press_start = now;
            s_long_sent = false;
        } else {                         // release
            if (!s_long_sent) {
                if (s_await_double && (now - s_pending_release) < DOUBLE_MS) {
                    s_await_double = false;
                    return BTN_DOUBLE;
                }
                s_await_double = true;
                s_pending_release = now;
            }
        }
    }

    // Long-press fires while still held.
    if (s_last && !s_long_sent && (now - s_press_start) > LONG_MS) {
        s_long_sent = true;
        s_await_double = false;
        return BTN_LONG;
    }

    // A lone press with no second within the window becomes a single.
    if (s_await_double && (now - s_pending_release) >= DOUBLE_MS) {
        s_await_double = false;
        return BTN_SINGLE;
    }
    return BTN_NONE;
}
