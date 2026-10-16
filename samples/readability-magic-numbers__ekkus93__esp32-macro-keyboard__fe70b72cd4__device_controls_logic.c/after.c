#include "device_controls_logic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool device_controls_level_is_pressed(int level, int active_level) {
    return (active_level == 0 || active_level == 1) && level == active_level;
}

bool device_controls_debounce_update(device_controls_debounce_t *button, bool sample) {
    if (button == NULL) {
        return false;
    }
    if (sample != button->candidate) {
        button->candidate = sample;
        button->candidate_count = 1U;
        return false;
    }
    if (button->candidate_count < DEVICE_CONTROLS_DEBOUNCE_SAMPLES) {
        ++button->candidate_count;
    }
    if (button->candidate_count == DEVICE_CONTROLS_DEBOUNCE_SAMPLES && button->stable != sample) {
        button->stable = sample;
        return sample;
    }
    return false;
}

/* Status-LED blink patterns as (period, on-time) in milliseconds. */
#define INDICATOR_BOOTING_PERIOD_MS 1000U
#define INDICATOR_BOOTING_ON_MS 250U
#define INDICATOR_EXECUTING_PERIOD_MS 200U
#define INDICATOR_EXECUTING_ON_MS 100U
#define INDICATOR_DEGRADED_PERIOD_MS 2000U
#define INDICATOR_DEGRADED_FIRST_ON_MS 250U
#define INDICATOR_DEGRADED_SECOND_ON_START_MS 500U
#define INDICATOR_DEGRADED_SECOND_ON_END_MS 750U
#define INDICATOR_FATAL_PERIOD_MS 500U
#define INDICATOR_FATAL_ON_MS 250U

bool device_controls_indicator_on(device_indicator_state_t state, uint32_t elapsed_ms) {
    switch (state) {
    case DEVICE_INDICATOR_READY:
        return true;
    case DEVICE_INDICATOR_BOOTING:
        return (elapsed_ms % INDICATOR_BOOTING_PERIOD_MS) < INDICATOR_BOOTING_ON_MS;
    case DEVICE_INDICATOR_EXECUTING:
        return (elapsed_ms % INDICATOR_EXECUTING_PERIOD_MS) < INDICATOR_EXECUTING_ON_MS;
    case DEVICE_INDICATOR_DEGRADED: {
        const uint32_t position = elapsed_ms % INDICATOR_DEGRADED_PERIOD_MS;
        return position < INDICATOR_DEGRADED_FIRST_ON_MS ||
               (position >= INDICATOR_DEGRADED_SECOND_ON_START_MS &&
                position < INDICATOR_DEGRADED_SECOND_ON_END_MS);
    }
    case DEVICE_INDICATOR_FATAL:
        return (elapsed_ms % INDICATOR_FATAL_PERIOD_MS) < INDICATOR_FATAL_ON_MS;
    default:
        return false;
    }
}
