#pragma once

#include "../tensor/tensor.hpp" // for Tensor
#include <algorithm>            // for fill, max
#include <cstddef>              // for size_t
#include <vector>               // for vector

namespace kaad::detail {

/**
 * @brief Computes the resulting shape of matrix multiplication using
 * broadcasting.
 * Supports matmul-style broadcasting:
 *   (n?,k) x (k,m?) => (n?,m?)
 * @param shape1 Shape of first matrix.
 * @param rank1 Number of dimensions of first matrix.
 * @param shape2 Shape of second matrix.
 * @param rank2 Number of dimensions of second matrix.
 * @param newShape Output array to hold the result shape.
 * @param newLen Total number of dimensions in the result.
 * @return true if matmul broadcasting is possible, false otherwise.
 */
inline bool combine_matrix(const int *shape1, std::size_t rank1,
                           const int *shape2, std::size_t rank2, int *newShape,
                           std::size_t newLen) {
    if (shape1[rank1 - 1] != shape2[rank2 - 2]) {
        return false;
    }
    std::fill(newShape, newShape + newLen, 0);

    newShape[newLen - 1] = shape2[rank2 - 1];
    newShape[newLen - 2] = shape1[rank1 - 2];

    int ind = newLen - 3;
    for (int i = 3; i <= static_cast<int>(newLen); i++, ind--) {
        int ind1 = rank1 - i;
        int ind2 = rank2 - i;
        if (ind1 >= 0 && ind2 >= 0) {
            if (shape1[ind1] != shape2[ind2] && shape1[ind1] != 1 &&
                shape2[ind2] != 1) {
                return false;
            }
            newShape[ind] = std::max(shape1[ind1], shape2[ind2]);
        } else {
            newShape[ind] = ind1 >= 0 ? shape1[ind1] : shape2[ind2];
        }
    }
    return true;
}

/**
 * @brief Computes stride and offset metadata for operations along a specific
 * tensor dimension.
 * @param A          Input tensor.
 * @param C          Output tensor (e.g., reduced along `dim`).
 * @param dim        The dimension along which the operation is applied.
 * @param D          (out) Number of dimensions.
 * @param A_offset   (out) Array storing the maximum valid offset per dimension
 * for A.
 * @param strideA    (out) Stride array for A.
 * @param strideC    (out) Stride array for C, adjusted to zero along `dim`.
 */
inline void along_dim_metadata_impl(Tensor &A, Tensor &C, int dim,
                                    std::size_t &D,
                                    std::vector<std::size_t> &A_offset,
                                    std::vector<int> &strideA,
                                    std::vector<int> &strideC) {
    D = A.rank();
    strideA = std::vector<int>(A.stride());

    // make sure stride[i] is 1 instead of 0 if shape[i] is 1 for
    // traversing in flexible function
    for (std::size_t i = 0; i < D; i++) {
        if (strideA[i] == 0 && A.shape()[i] == 1) {
            strideA[i] = 1;
        }
    }

    strideC = std::vector<int>(C.stride());

    // adjust strideC if keep_rank was not set
    if (A.rank() > C.rank()) {

        strideC.push_back(0);

        std::move(strideC.begin() + dim, strideC.end() - 1,
                  strideC.begin() + dim + 1);
    }

    strideC[dim] = 0;
    for (int i = 0; i < dim; i++) {
        strideC[i] /= A.shape()[dim];
    }

    A_offset.resize(D);
    for (std::size_t i = 0; i < D; i++) {
        A_offset[i] = A.shape()[i] * strideA[i];
    }
}

} // namespace kaad::detail
