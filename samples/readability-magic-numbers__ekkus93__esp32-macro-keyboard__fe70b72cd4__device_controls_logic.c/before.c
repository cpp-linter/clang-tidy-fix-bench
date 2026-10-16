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

bool device_controls_indicator_on(device_indicator_state_t state, uint32_t elapsed_ms) {
    switch (state) {
    case DEVICE_INDICATOR_READY:
        return true;
    case DEVICE_INDICATOR_BOOTING:
        return (elapsed_ms % 1000U) < 250U;
    case DEVICE_INDICATOR_EXECUTING:
        return (elapsed_ms % 200U) < 100U;
    case DEVICE_INDICATOR_DEGRADED: {
        const uint32_t position = elapsed_ms % 2000U;
        return position < 250U || (position >= 500U && position < 750U);
    }
    case DEVICE_INDICATOR_FATAL:
        return (elapsed_ms % 500U) < 250U;
    default:
        return false;
    }
}
