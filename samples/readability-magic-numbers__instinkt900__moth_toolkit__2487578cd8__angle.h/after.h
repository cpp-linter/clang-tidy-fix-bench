#pragma once

#include "moth/core/transform.h"

#include <cmath>

namespace moth::core {
    namespace {
        constexpr float kPi = 3.14159265f;
        constexpr float kTwoPi = 2.0f * kPi;
        /// Degrees in a half turn, the scale factor between the two units.
        constexpr float kDegreesPerHalfTurn = 180.0f;
    }

    /// @brief Converts an angle in degrees to radians.
    ///
    /// Every toolkit API takes and returns radians; convert at the boundary with
    /// degree-based sources such as authored data files.
    constexpr float DegToRad(float degrees) {
        return degrees * (kPi / kDegreesPerHalfTurn);
    }

    /// @brief Converts an angle in radians to degrees, e.g. for display or serialisation.
    constexpr float RadToDeg(float radians) {
        return radians * (kDegreesPerHalfTurn / kPi);
    }

    /// @brief Wraps an angle in radians to the range [-π, π].
    inline float WrapAngleRadians(float radians) {
        radians = std::fmod(radians + kPi, kTwoPi);
        if (radians < 0.0f) {
            radians += kTwoPi;
        }
        return radians - kPi;
    }

    /// @brief Returns the shortest signed difference (radians) from @p from to @p to, in [-π, π].
    inline float AngleDeltaRadians(float from, float to) {
        return WrapAngleRadians(to - from);
    }

    /// @brief Linearly interpolates two angles (radians) along the shortest arc.
    inline float LerpAngleRadians(float from, float to, float t) {
        return from + (AngleDeltaRadians(from, to) * t);
    }
}
