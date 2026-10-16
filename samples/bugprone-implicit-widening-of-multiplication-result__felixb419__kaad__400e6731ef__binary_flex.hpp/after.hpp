#pragma once

#include "../../functions/adjoint.hpp" // for flexible, flexible_fn
#include "../../functions/primal.hpp"  // for flexible, flexible_fn
#include "../../tensor/tensor.hpp"     // for Tensor
#include "../dispatchers.hpp"          // for MAX_NDIMS, get_flex...
#include "inode.hpp"                   // for INode
#include <cstddef>                     // for size_t
#include <span>                        // for span
#include <vector>                      // for vector

namespace kaad {

template <class Kernel> class Node_binary_flex;

/**
 * @brief A binary_flex operation node in a computation graph.
 * @ingroup nodes
 * @see functions::primal::binary::flexible
 * @see functions::adjoint::binary::flexible
 * @tparam Kernel A kernel struct providing `Op` and `Grad` types for the
 * operation.
 */
template <class Kernel> class Node_binary_flex : public INode {
  private:
    functions::primal::binary::flexible_fn<Kernel> forward_op =
        functions::primal::binary::flexible<Kernel>; ///< Function pointer to
                                                     ///< the value operation.

    functions::adjoint::binary::flexible_fn<Kernel> backward_op =
        functions::adjoint::binary::flexible<
            Kernel>; ///< Function pointer to the gradient operation.

    INode *lhs = nullptr; ///< Pointer to the first input Node.
    INode *rhs = nullptr; ///< Pointer to the second input Node.

    std::vector<int> lhs_stride;   ///< stride Array for A.
    std::vector<int> rhs_stride;   ///< stride Array for B.
    std::vector<int> value_stride; ///< stride Array for C.
    std::vector<std::size_t>
        C_offset;               ///< Per-dim offset to the end of C buffer.
    std::size_t value_rank = 0; ///< Number of the dimensions of the C tensor.

    void metadata() {
        // compute metadata
        Tensor &lhs = this->lhs->value();
        Tensor &rhs = this->rhs->value();
        Tensor &value = this->value();

        this->value_rank = value.rank();
        this->lhs_stride.resize(this->value_rank);
        this->rhs_stride.resize(this->value_rank);
        this->value_stride.resize(this->value_rank);

        int idx, idxA, idxB, idxC;
        for (std::size_t i = 1; i <= this->value_rank; i++) {
            idx = this->value_rank - i;
            idxA = lhs.rank() - i;
            this->lhs_stride[idx] = idxA >= 0 ? lhs.stride()[idxA] : 0;
            idxB = rhs.rank() - i;
            this->rhs_stride[idx] = idxB >= 0 ? rhs.stride()[idxB] : 0;
            idxC = value.rank() - i;
            this->value_stride[idx] = idxC >= 0 ? value.stride()[idxC] : 0;
            // make sure value_stride[idx] is 1 instead of 0 if value.shape[idx]
            // is 1 for traversing in flexible function
            if (this->value_stride[idx] == 0 && value.shape()[idxC] == 1) {
                this->value_stride[idx] = 1;
            }
        }

        this->C_offset.resize(this->value_rank);
        for (std::size_t i = 0; i < this->value_rank; i++) {
            this->C_offset[i] = static_cast<std::size_t>(value.shape()[i]) *
                                this->value_stride[i];
        }

        // assign compile-time recursive function
        if (this->value_rank <= Dispatchers::MAX_NDIMS) {
            this->forward_op =
                Dispatchers::get_flexOp<Kernel>()[this->value_rank];
            this->backward_op =
                Dispatchers::get_flexGrad<Kernel>()[this->value_rank];
        }
    }

  public:
    /**
     * @brief Constructs a binary_flex operation node with binary_flex operation
     * @ingroup nodes
     * and gradient.
     * @param A_ptr Pointer to the first input node.
     * @param B_ptr Pointer to the second input node.
     * @param value_shape Shape of the value and gradient tensors.
     */
    Node_binary_flex(INode *lhs_ptr, INode *rhs_ptr,
                     std::span<const int> value_shape)
        : INode(value_shape, false), lhs(lhs_ptr), rhs(rhs_ptr) {

        this->metadata();
    }

    /**
     * @brief Returns the type of the node as a string.
     * @ingroup nodes
     */
    const char *node_type() const noexcept override {
        return "Node_binary_flex";
    }

    /**
     * @brief Evaluates the binary_flex operation by calling forward_op, if not
     * @ingroup nodes
     * already evaluated.
     */
    inline void eval() override {
        if (!this->evaluated()) {
            this->lhs->eval();
            this->rhs->eval();

            forward_op(this->lhs->value().data(), this->rhs->value().data(),
                       this->value().data(), lhs_stride.data(),
                       rhs_stride.data(), value_stride.data(), C_offset.data(),
                       value_rank);
            this->evaluated_ = true;
        }
    }

    /**
     * @brief Propagates gradients back through the binary_flex operation, by
     * @ingroup nodes
     * calling backward_op.
     */
    inline void getGrad() override {
        backward_op(this->lhs->value().data(), this->lhs->gradient().data(),
                    this->rhs->value().data(), this->rhs->gradient().data(),
                    this->value().data(), this->gradient().data(),
                    lhs_stride.data(), rhs_stride.data(), value_stride.data(),
                    C_offset.data(), value_rank);

        if (!this->lhs->isInput()) {
            this->lhs->getGrad();
        }
        if (!this->rhs->isInput()) {
            this->rhs->getGrad();
        }
    }
};

} // namespace kaad
