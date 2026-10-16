#pragma once

#include <cmath>   // for log, pow, exp, sqrt
#include <cstdlib> // for abs
#include <limits>  // for numeric_limits

/**
 * @namespace kaad::Kernels
 * @brief Contains elementary operation kernels and their corresponding
 * gradients.
 */
namespace kaad::Kernels {

/**
 * @brief Binary kernel for addition.
 * @tparam T The scalar type.
 */
template <typename T> struct Add {
    using value_type = T;

    /**
     * @brief C = lhs + B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept { res = lhs + rhs; }
    /**
     * @brief Computes the gradient of an addition.
     */
    constexpr static void Grad([[maybe_unused]] T lhs, T &d_lhs,
                               [[maybe_unused]] T rhs, T &d_rhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs += d_res;
        d_rhs += d_res;
    }
};

/**
 * @brief Binary kernel for subtraction.
 * @tparam T The scalar type.
 */
template <typename T> struct Sub {
    using value_type = T;

    /**
     * @brief C = lhs - B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept { res = lhs - rhs; }
    /**
     * @brief Computes the gradient of a subtraction.
     */
    constexpr static void Grad([[maybe_unused]] T lhs, T &d_lhs,
                               [[maybe_unused]] T rhs, T &d_rhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs += d_res;
        d_rhs -= d_res;
    }
};

/**
 * @brief Binary kernel for multiplication.
 * @tparam T The scalar type.
 */
template <typename T> struct Mul {
    using value_type = T;

    /**
     * @brief C = lhs * B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept { res = lhs * rhs; }
    /**
     * @brief Computes the gradient a multiplication.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs += d_res * rhs;
        d_rhs += d_res * lhs;
    }
};

/**
 * @brief Binary kernel for division.
 * @note In most applications @ref safe_Div is preferable.
 * @tparam T The scalar type.
 */
template <typename T> struct Div {
    using value_type = T;

    const static T epsilon =
        static_cast<T>(1000) * std::numeric_limits<T>::epsilon();

    /**
     * @brief C = lhs / B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept { res = lhs / rhs; }

    /**
     * @brief Computes the gradient of a division.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs, T res,
                               T d_res) noexcept {
        d_lhs += d_res * (1 / rhs);
        d_rhs -= d_res * (lhs / (rhs * rhs));
    }
};

/**
 * @brief Binary kernel for power.
 * @note In most applications @ref safe_Pow is preferable.
 * @tparam T The scalar type.
 */
template <typename T> struct Pow {
    using value_type = T;

    const static T epsilon =
        static_cast<T>(1000) * std::numeric_limits<T>::epsilon();

    static constexpr T max_finite = std::numeric_limits<T>::max();
    static constexpr T max_exp = std::log(max_finite);
    static constexpr T min_exp = -max_exp;

    /**
     * @brief C = lhs ^ B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept {
        res = std::pow(lhs, rhs);
    }
    /**
     * @brief Computes the gradient of an exponentiation.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs, T res,
                               T d_res) noexcept {
        d_lhs += d_res * rhs * std::pow(lhs, rhs - 1);
        d_rhs += d_res * res * std::log(lhs);
    }
};

/**
 * @brief Binary kernel for dot.
 * @tparam T The scalar type.
 */
template <typename T> struct Dot {
    using value_type = T;

    /**
     * @brief C += lhs * B
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept {
        res += lhs * rhs;
    }
    /**
     * @brief Computes the gradient of a dot-product.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs, T res,
                               T d_res) noexcept {
        d_lhs += d_res * rhs;
        d_rhs += d_res * lhs;
    }
};

/**
 * @brief Binary kernel for minimum.
 * @tparam T The scalar type.
 */
template <typename T> struct Min {
    using value_type = T;

    /**
     * @brief C = min(lhs,B)
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept {
        res = lhs < rhs ? lhs : rhs;
    }
    /**
     * @brief Computes the gradient of the min function.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        bool smaller = lhs <= rhs;
        d_lhs += smaller ? d_res : 0;
        d_rhs += smaller ? 0 : d_res;
    }
};

/**
 * @brief Binary kernel for maximum.
 * @tparam T The scalar type.
 */
template <typename T> struct Max {
    using value_type = T;

    /**
     * @brief C = max(lhs,B)
     */
    constexpr static void Op(T lhs, T rhs, T &res) noexcept {
        res = lhs > rhs ? lhs : rhs;
    }
    /**
     * @brief Computes the gradient of the max function.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T rhs, T &d_rhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        bool bigger = lhs >= rhs;
        d_lhs += bigger ? d_res : 0;
        d_rhs += bigger ? 0 : d_res;
    }
};

/**
 * @brief Binary kernel for no-operation.
 * @tparam T The scalar type.
 */
template <typename T> struct NoOp {
    using value_type = T;

    /**
     * @brief C = lhs
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = lhs; }

    /**
     * @brief d_lhs += dC
     */
    constexpr static void Grad([[maybe_unused]] T lhs, T &d_lhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs += d_res;
    }
};

/**
 * @brief Unary kernel for summation.
 * @tparam T The scalar type.
 */
template <typename T> struct Sum {
    using value_type = T;

    /**
     * @brief C += lhs
     */
    constexpr static void Op(T lhs, T &res) noexcept { res += lhs; }
    /**
     * @brief Computes the gradient of the summation.
     */
    constexpr static void Grad([[maybe_unused]] T lhs, T &d_lhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs += d_res;
    }
};

/**
 * @brief Unary kernel for negation.
 * @tparam T The scalar type.
 */
template <typename T> struct Neg {
    using value_type = T;

    /**
     * @brief C = -lhs
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = -lhs; }
    /**
     * @brief Computes the gradient of the negation.
     */
    constexpr static void Grad([[maybe_unused]] T lhs, T &d_lhs,
                               [[maybe_unused]] T res, T d_res) noexcept {
        d_lhs -= d_res;
    }
};

/**
 * @brief Unary kernel for square.
 * @tparam T The scalar type.
 */
template <typename T> struct Square {
    using value_type = T;

    /**
     * @brief C = lhs ^ 2
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = lhs * lhs; }
    /**
     * @brief Computes the gradient of the square.
     */
    constexpr static void Grad(T lhs, T &d_lhs, [[maybe_unused]] T res,
                               T d_res) noexcept {
        d_lhs += d_res * 2 * lhs;
    }
};

/**
 * @brief Unary kernel for squareroot.
 * @note In most applications @ref safe_Sqrt is preferable.
 * @tparam T The scalar type.
 */
template <typename T> struct Sqrt {
    using value_type = T;

    const static T epsilon =
        static_cast<T>(1000) * std::numeric_limits<T>::epsilon();
    /**
     * @brief C = sqrt(lhs)
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = std::sqrt(lhs); }

    /**
     * @brief Computes the gradient of the squareroot
     */
    constexpr static void Grad(T lhs, T &d_lhs, T res, T d_res) noexcept {
        d_lhs += d_res / (2 * res);
    }
};

/**
 * @brief Unary kernel for logarithm.
 * @note In most applications @ref safe_Log is preferable.
 * @tparam T The scalar type.
 */
template <typename T> struct Log {
    using value_type = T;

    const static T epsilon =
        static_cast<T>(1000) * std::numeric_limits<T>::epsilon();
    /**
     * @brief C = log(lhs)
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = std::log(lhs); }
    /**
     * @brief Computes the gradient of the logarithm
     */
    constexpr static void Grad(T lhs, T &d_lhs, T res, T d_res) noexcept {
        d_lhs += d_res / lhs;
    }
};

/**
 * @brief Unary kernel for exponent.
 * @note In most applications @ref safe_Exp is preferable.
 * @tparam T The scalar type.
 */
template <typename T> struct Exp {
    using value_type = T;

    inline static T max_exp = std::log(std::numeric_limits<T>::max());
    inline static T min_exp = std::log(std::numeric_limits<T>::min());
    constexpr static void Op(T lhs, T &res) noexcept {
        /**
         * @brief C = e^lhs
         * @if NO_STABLE_EXP is not defined a numerically safe version is
         * used instead.
         */
        res = std::exp(lhs);
    }
    /**
     * @brief Computes the gradient of the exp function.
     */
    constexpr static void Grad(T lhs, T &d_lhs, T res, T d_res) noexcept {
        d_lhs += d_res * res;
    }
};

/**
 * @brief Unary kernel for abs.
 * @tparam T The scalar type.
 */
template <typename T> struct Abs {
    using value_type = T;

    /**
     * @brief C = abs(lhs)
     */
    constexpr static void Op(T lhs, T &res) noexcept { res = std::abs(lhs); }
    /**
     * @brief Computes the gradient of the abs function.
     */
    constexpr static void Grad(T lhs, T &d_lhs, [[maybe_unused]] T res,
                               T d_res) noexcept {
        d_lhs += d_res * (lhs > 0 ? 1 : -1);
    }
};

} // namespace kaad::Kernels
