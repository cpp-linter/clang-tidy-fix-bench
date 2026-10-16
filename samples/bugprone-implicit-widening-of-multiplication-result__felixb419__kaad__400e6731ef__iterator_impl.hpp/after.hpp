#pragma once

#include "../scalar.hpp" // for Scalar
#include <algorithm>     // for copy
#include <iterator>      // for bidirectional_iterator_tag
#include <type_traits>   // for conditional_t
#include <vector>        // for vector

namespace kaad {

class Tensor;

template <bool isConst> class iterator_impl {
  private:
    using Tensor_reference =
        std::conditional_t<isConst, const Tensor &, Tensor &>;
    Tensor_reference origin_;        ///< Origin tensor of the iterator.
    std::vector<int> cords_;         ///< Per-dim coordinates of the element.
    const std::vector<int> &shape_;  ///< Shape of the tensor.
    const std::vector<int> &stride_; ///< Stride array of the tensor.

  public:
    using iterator_concept = std::bidirectional_iterator_tag;
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Scalar;
    using difference_type = std::ptrdiff_t;
    using pointer = std::conditional_t<isConst, const Scalar *, Scalar *>;
    using reference = std::conditional_t<isConst, const Scalar &, Scalar &>;

    iterator_impl(Tensor_reference &origin, std::vector<int> &&cords)
        : origin_(origin), cords_(cords), shape_(origin_.shape()),
          stride_(origin_.stride()) {}

    const Tensor &origin() { return this->origin_; }

    reference operator*() const {
        int idx = 0;
        for (std::size_t i = 0; i < this->origin_.rank(); i++) {
            idx += this->cords_[i] * this->stride_[i];
        }
        return this->origin_.data()[idx];
    }

    iterator_impl &operator++() {
        if (this->origin_.rank() == 0) {
            this->cords_[0] = 1;
            return *this;
        }

        int rank = static_cast<int>(this->origin_.rank() - 1);

        this->cords_[rank]++;

        while (this->cords_[rank] >= this->shape_[rank]) {

            this->cords_[rank] = 0;
            rank--;
            if (rank >= 0) {
                this->cords_[rank]++;

            } else {
                // increment every cord but the last, so iterator points one
                // past end and return.
                std::copy(this->shape_.begin(), this->shape_.end(),
                          this->cords_.begin());
                for (std::size_t i = 0; i < this->origin_.rank() - 1; i++) {
                    this->cords_[i]--;
                }
                break;
            }
        }

        return *this;
    }

    iterator_impl operator++(int) {
        iterator_impl old = *this;
        ++(*this);
        return old;
    }

    iterator_impl &operator--() {
        int rank = static_cast<int>(this->origin_.rank() - 1);

        this->cords_[rank]--;

        while (this->cords_[rank] < 0) {

            this->cords_[rank] = this->shape_[rank] - 1;
            if (rank >= 0) {
                rank--;
                this->cords_[rank]--;

            } else {
                // set cords to all 0 and return
                std::fill(this->cords_.begin(), this->cords_.end(), 0);
                break;
            }
        }

        return *this;
    }

    iterator_impl operator--(int) {
        iterator_impl old = *this;
        --(*this);
        return old;
    }

    bool operator==(const iterator_impl &other) const {
        return (&this->origin_ == &other.origin_) &&
               std::equal(this->cords_.begin(), this->cords_.end(),
                          other.cords_.begin());
    }

    bool operator!=(const iterator_impl &other) const {
        return !(*this == other);
    }
};

} // namespace kaad
