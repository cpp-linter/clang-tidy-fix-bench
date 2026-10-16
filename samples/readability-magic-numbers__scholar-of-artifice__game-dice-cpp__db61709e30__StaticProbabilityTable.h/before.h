//
// Copyright 2026 scholar-of-artifice
//
// Licensed under the MIT License
//
// Copyright (c) 2026 scholar-of-artifice
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef GAME_DICE_CPP_SRC_STATICPROBABILITYTABLE_H
#define GAME_DICE_CPP_SRC_STATICPROBABILITYTABLE_H
#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>

namespace game_dice_cpp {

template <size_t NumberOfOutcomes>
class StaticProbabilityTable {
 private:
  std::array<int, NumberOfOutcomes> thresholds_;

  constexpr explicit StaticProbabilityTable(
      const std::array<int, NumberOfOutcomes>& thresholds)
      : thresholds_(thresholds) {}

 public:
  //
  [[nodiscard]] static constexpr std::optional<
      game_dice_cpp::StaticProbabilityTable<NumberOfOutcomes>>
  Make(const std::array<int, NumberOfOutcomes>& input_weights) {
    // create a local copy to work with
    std::array<int, NumberOfOutcomes> weights = input_weights;
    // transform in-place only non-negative weights
    std::ranges::transform(weights, weights.begin(),
                           [](int weight) { return std::max(weight, 0); });
    // accumulate with check-as-you-go overflow check
    // use std::optional<int> to carry the valid state through the loop
    std::optional<int> total_weight = std::accumulate(
        weights.begin(), weights.end(), std::optional<int>(0),
        [](std::optional<int> accumulated, int weight) -> std::optional<int> {
          // if a previous step failed...
          if (!accumulated) {
            return std::nullopt;
          }
          // check for overflow before it happens
          if (weight > std::numeric_limits<int>::max() - *accumulated) {
            return std::nullopt;
          }
          return *accumulated + weight;
        });
    // validation
    if (!total_weight.has_value() || *total_weight <= 0) {
      return std::nullopt;
    }
    // calculate thresholds
    std::array<int, NumberOfOutcomes> thresholds{};
    std::partial_sum(weights.begin(), weights.end(), thresholds.begin());
    // check if the thresholds do not exist or sum to nothing
    if (thresholds.empty() || thresholds.back() <= 0) {
      return std::nullopt;
    }
    // construct and return
    return StaticProbabilityTable(thresholds);
  }
  [[nodiscard]] constexpr int GetTotalWeight() const {
    return thresholds_.back();
  }
  [[nodiscard]] constexpr int At(int value) const {
    // small table optimization
    if constexpr (NumberOfOutcomes <= 16) {
      // linear search for the value
      const auto iter =
          std::find_if(thresholds_.begin(), thresholds_.end(),
                       [value](int threshold) { return threshold >= value; });
      if (iter == thresholds_.end()) {
        return static_cast<int>(thresholds_.size() - 1);
      } else {
        return static_cast<int>(std::distance(thresholds_.begin(), iter));
      }
    } else {
      // binary search for the value
      const auto iter =
          std::lower_bound(thresholds_.begin(), thresholds_.end(), value);
      // clamp value within range of table
      if (iter == thresholds_.end()) {
        // this case happens when the input value is greater than the
        // total_weight
        return static_cast<int>(thresholds_.size() - 1);
      }
      return static_cast<int>(std::distance(thresholds_.begin(), iter));
    }
  }
};

}  // namespace game_dice_cpp

#endif  // GAME_DICE_CPP_SRC_STATICPROBABILITYTABLE_H
