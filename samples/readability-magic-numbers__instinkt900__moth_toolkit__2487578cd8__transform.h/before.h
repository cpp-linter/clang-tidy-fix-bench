#pragma once

#include "moth/core/vector.h"

#include <array>
#include <cmath>

namespace moth::core {
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers)
    /// @brief The default rotation pivot: the centre of a node's bounds.
    inline FloatVec2 const kDefaultPivot = { 0.5f, 0.5f };
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers)

    /**
     * @brief A standard 4×4 row-major transform matrix.
     *
     * Row i, column j is accessed as m[i][j].
     * Transforms a 2D point (x, y) as (z=0, w=1):
     *   x' = m[0][0]*x + m[0][1]*y + m[0][3]
     *   y' = m[1][0]*x + m[1][1]*y + m[1][3]
     *
     * The identity transform leaves points unchanged.
     * Composition (operator*) applies the right-hand operand first.
     */
    struct FloatMat4x4 {
        std::array<std::array<float, 4>, 4> m = {{
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f, 1.0f },
        }};

        /// @brief Returns the identity transform.
        static FloatMat4x4 Identity() { return {}; }

        /// @brief Returns a pure XY translation transform.
        static FloatMat4x4 Translation(FloatVec2 offset) {
            FloatMat4x4 t;
            t.m[0][3] = offset.x;
            t.m[1][3] = offset.y;
            return t;
        }

        /// @brief Returns a pure XY scale transform.
        static FloatMat4x4 Scale(FloatVec2 scale) {
            FloatMat4x4 s;
            s.m[0][0] = scale.x;
            s.m[1][1] = scale.y;
            return s;
        }

        /// @brief Returns a clockwise rotation around Z in radians, pivoting around @p pivot (in the same space as the points being transformed).
        ///
        /// "Clockwise" is in screen space (y-down). The same matrix is
        /// counter-clockwise in standard math convention (y-up). It matches
        /// @c Rotate2D exactly.
        static FloatMat4x4 Rotation(float radians, FloatVec2 pivot) {
            float const cosA = std::cos(radians);
            float const sinA = std::sin(radians);
            FloatMat4x4 r;
            r.m[0][0] =  cosA;  r.m[0][1] = -sinA;  r.m[0][3] = (pivot.x * (1.0f - cosA)) + (pivot.y * sinA);
            r.m[1][0] =  sinA;  r.m[1][1] =  cosA;  r.m[1][3] = (pivot.y * (1.0f - cosA)) - (pivot.x * sinA);
            return r;
        }

        /// @brief Composes two transforms: applies @p rhs first, then @c *this.
        FloatMat4x4 operator*(FloatMat4x4 const& rhs) const {
            FloatMat4x4 result;
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    result.m[i][j] = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        result.m[i][j] += m[i][k] * rhs.m[k][j];
                    }
                }
            }
            return result;
        }

        /// @brief Applies the transform to a 2D point (z=0, w=1).
        FloatVec2 TransformPoint(FloatVec2 const& p) const {
            return {
                (m[0][0] * p.x) + (m[0][1] * p.y) + m[0][3],
                (m[1][0] * p.x) + (m[1][1] * p.y) + m[1][3],
            };
        }

        /// @brief Extracts the clockwise Z-rotation angle in radians encoded in the matrix.
        float GetRotation() const {
            return std::atan2(m[1][0], m[0][0]);
        }

        /**
         * @brief Returns the inverse of this transform.
         *
         * Inverts the 2x2 linear part and the translation, so it is exact for any
         * affine transform (rotation + scale + shear + translation), not just
         * rigid-body transforms. If the linear part is singular (det ~ 0) the
         * inverse does not exist and @c Identity() is returned.
         */
        FloatMat4x4 Invert() const {
            FloatMat4x4 result;
            float const a = m[0][0];
            float const b = m[0][1];
            float const c = m[1][0];
            float const d = m[1][1];
            float const tx = m[0][3];
            float const ty = m[1][3];

            float const det = (a * d) - (b * c);
            if (std::abs(det) < 1e-8f) {
                return Identity();
            }
            float const invDet = 1.0f / det;

            result.m[0][0] = d * invDet;
            result.m[0][1] = -b * invDet;
            result.m[1][0] = -c * invDet;
            result.m[1][1] = a * invDet;
            result.m[0][3] = -((result.m[0][0] * tx) + (result.m[0][1] * ty));
            result.m[1][3] = -((result.m[1][0] * tx) + (result.m[1][1] * ty));
            return result;
        }
    };
}
