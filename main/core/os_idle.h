#pragma once

// Idle tracking for Settings > Auto-dim. Pure logic (no ESP-IDF, no LVGL) so it
// is tested on the host (core/tests/idle). The OS task feeds it the time of
// every button press and polls it on its tick; the OS does the actual dimming.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t timeout_s;      // 0 = never dim
    int64_t last_input_ms;
    bool dimmed;
} os_idle_t;

// Start counting from now_ms with the given timeout (0 = off).
void os_idle_init(os_idle_t *t, int32_t timeout_s, int64_t now_ms);

// Change the timeout (the setting changed). Restarts the count; stays dimmed
// only until the next input, so call os_idle_input first if the screen is dim.
void os_idle_set_timeout(os_idle_t *t, int32_t timeout_s, int64_t now_ms);

// A button was pressed. Returns true if the screen was dim: restore brightness.
bool os_idle_input(os_idle_t *t, int64_t now_ms);

// Poll. Returns true once, when the idle time has just passed: dim the screen.
bool os_idle_poll(os_idle_t *t, int64_t now_ms);
