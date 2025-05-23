/**
 * @file ShapeFunctions.h
 * @author Quoc-Minh Ton-That (tonthat.quocminh@gmail.com)
 * @brief FEM shape functions and gradients
 * @date 2025-02-11
 *
 * @copyright Copyright (c) 2025
 *
 */

#ifndef PBAT_FEM_SHAPE_FUNCTIONS_H
#define PBAT_FEM_SHAPE_FUNCTIONS_H

#include "Concepts.h"
#include "Jacobian.h"

#include <Eigen/Cholesky>
#include <Eigen/LU>
#include <exception>
#include <fmt/core.h>
#include <optional>
#include <pbat/Aliases.h>
#include <pbat/profiling/Profiling.h>
#include <string>
#include <tbb/parallel_for.h>

namespace pbat {
namespace fem {

/**
 * @brief Computes shape functions at element quadrature points for a polynomial quadrature rule of
 * order QuadratureOrder
 * @tparam TElement Element type
 * @tparam QuadratureOrder Quadrature order
 * @return The shape function values of each node at quadrature points, stored in a matrix
 * of dimensions `|# element nodes| x |# quad.pts.|`
 */
template <CElement TElement, int QuadratureOrder>
auto ShapeFunctions()
    -> Matrix<TElement::kNodes, TElement::template QuadratureType<QuadratureOrder>::kPoints>
{
    using QuadratureRuleType = typename TElement::template QuadratureType<QuadratureOrder>;
    using ElementType        = TElement;
    auto const Xg            = common::ToEigen(QuadratureRuleType::points)
                        .reshaped(QuadratureRuleType::kDims + 1, QuadratureRuleType::kPoints)
                        .template bottomRows<QuadratureRuleType::kDims>();
    Matrix<ElementType::kNodes, QuadratureRuleType::kPoints> Ng{};
    for (auto g = 0; g < QuadratureRuleType::kPoints; ++g)
    {
        Ng.col(g) = ElementType::N(Xg.col(g));
    }
    return Ng;
}

/**
 * @brief Constructs a shape function matrix \f$ \mathbf{N} \f$ for a given mesh, i.e. at the
 * element quadrature points.
 * @tparam QuadratureOrder Quadrature order
 * @tparam TMesh Mesh type
 * @param mesh FEM mesh
 * @return `|# elements * # quad.pts.| x |# nodes|` shape function matrix
 */
template <int QuadratureOrder, CMesh TMesh>
CSRMatrix ShapeFunctionMatrix(TMesh const& mesh)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionMatrix");
    using ElementType               = typename TMesh::ElementType;
    auto const Ng                   = ShapeFunctions<ElementType, QuadratureOrder>();
    auto const numberOfNodes        = mesh.X.cols();
    auto const numberOfElements     = mesh.E.cols();
    auto const numberOfQuadPoints   = Ng.cols();
    auto const m                    = numberOfQuadPoints * numberOfElements;
    auto const n                    = numberOfNodes;
    auto constexpr kNodesPerElement = ElementType::kNodes;
    CSRMatrix N(m, n);
    N.reserve(IndexVectorX::Constant(m, kNodesPerElement));
    for (auto e = 0; e < numberOfElements; ++e)
    {
        auto const nodes = mesh.E.col(e);
        for (auto g = 0; g < numberOfQuadPoints; ++g)
        {
            auto const row = e * numberOfQuadPoints + g;
            for (auto i = 0; i < Ng.rows(); ++i)
            {
                auto const col     = nodes(i);
                N.insert(row, col) = Ng(i, g);
            }
        }
    }
    return N;
}

/**
 * @brief Constructs a shape function matrix \f$ \mathbf{N} \f$ for a given mesh, i.e. at the
 * given evaluation points.
 * @tparam TMesh Mesh type
 * @tparam TDerivedE Eigen type
 * @tparam TDerivedX Eigen type
 * @param mesh FEM mesh
 * @param eg `|# quad.pts.|` array of elements associated with quadrature points
 * @param Xg `|# dims|x|# quad.pts.|` array of quadrature points in reference space
 * @param bXgInReferenceSpace Consider evaluation points Xg have coordinates in reference space
 * @return `|# quad.pts.| x |# nodes|` shape function matrix
 */
template <CMesh TMesh, class TDerivedE, class TDerivedX>
CSRMatrix ShapeFunctionMatrix(
    TMesh const& mesh,
    Eigen::DenseBase<TDerivedE> const& eg,
    Eigen::MatrixBase<TDerivedX> const& Xg,
    bool bXgInReferenceSpace = false)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionMatrix");
    using ElementType = typename TMesh::ElementType;

    MatrixX Xi{};
    if (not bXgInReferenceSpace)
    {
        // NOTE: Should expose the "iterations" parameter to parent function
        Xi = ReferencePositions(mesh, eg, Xg);
    }
    auto const numberOfNodes        = mesh.X.cols();
    auto const numberOfQuadPoints   = Xg.cols();
    auto const m                    = numberOfQuadPoints;
    auto const n                    = numberOfNodes;
    auto constexpr kNodesPerElement = ElementType::kNodes;
    CSRMatrix N(m, n);
    N.reserve(IndexVectorX::Constant(m, kNodesPerElement));
    for (auto g = 0; g < numberOfQuadPoints; ++g)
    {
        auto const e     = eg(g);
        auto const nodes = mesh.E.col(e);
        auto Ng = bXgInReferenceSpace ? ElementType::N(Xg.col(g)) : ElementType::N(Xi.col(g));
        for (auto i = 0; i < kNodesPerElement; ++i)
        {
            N.insert(g, nodes(i)) = Ng(i);
        }
    }
    return N;
}

/**
 * @brief Compute shape functions at the given reference positions
 * @tparam TElement Element type
 * @tparam TDerivedXi Eigen dense expression type for reference positions
 * @param Xi `|# dims|x|# quad.pts.|` evaluation points
 * @return `|# element nodes| x |Xi.cols()|` matrix of nodal shape functions at reference
 * points Xi
 */
template <CElement TElement, class TDerivedXi>
MatrixX ShapeFunctionsAt(Eigen::DenseBase<TDerivedXi> const& Xi)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionsAt");
    using ElementType = TElement;
    if (Xi.rows() != ElementType::kDims)
    {
        std::string const what = fmt::format(
            "Expected evaluation points in d={} dimensions, but got Xi.rows()={}",
            ElementType::kDims,
            Xi.rows());
        throw std::invalid_argument(what);
    }
    MatrixX N(ElementType::kNodes, Xi.cols());
    tbb::parallel_for(Index{0}, Index{Xi.cols()}, [&](Index i) {
        N.col(i) = ElementType::N(Xi.col(i));
    });
    return N;
}

/**
 * @brief Compute shape functions at the given reference or domain space positions
 * @tparam TMesh Mesh type
 * @tparam TDerivedE Eigen dense expression type for element indices
 * @tparam TDerivedXg Eigen dense expression type for evaluation points
 * @param mesh FEM mesh
 * @param eg `|# quad.pts.|` array of elements associated with quadrature points
 * @param Xg `|# dims|x|# quad.pts.|` evaluation points
 * @param bXgInReferenceSpace Consider evaluation points Xg as having coordinates in reference space
 * @return `|# element nodes| x |Xg.cols()|` matrix of nodal shape functions at reference
 * points Xg
 */
template <CMesh TMesh, class TDerivedE, class TDerivedXg>
MatrixX ShapeFunctionsAt(
    TMesh const& mesh,
    Eigen::DenseBase<TDerivedE> const& eg,
    Eigen::MatrixBase<TDerivedXg> const& Xg,
    bool bXgInReferenceSpace = false)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionsAt");
    using ElementType = typename TMesh::ElementType;
    if (bXgInReferenceSpace)
        return ShapeFunctionsAt<ElementType>(Xg);
    // NOTE: Should expose the "iterations" parameter to parent function
    return ShapeFunctionsAt<ElementType>(ReferencePositions(mesh, eg, Xg));
}

/**
 * @brief Integrate shape functions on each element
 * @tparam TDerived Eigen dense expression type for element jacobian determinants
 * @tparam TMesh Mesh type
 * @tparam QuadratureOrder Quadrature order
 * @param mesh FEM mesh
 * @param detJe `|# quad.pts.| x |# elements|` matrix of element jacobian determinants
 * @return `|# element nodes|x|# elements|`
 */
template <int QuadratureOrder, CMesh TMesh, class TDerived>
MatrixX IntegratedShapeFunctions(TMesh const& mesh, Eigen::DenseBase<TDerived> const& detJe)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.IntegratedShapeFunctions");
    using MeshType           = TMesh;
    using ElementType        = typename MeshType::ElementType;
    using QuadratureRuleType = typename ElementType::template QuadratureType<QuadratureOrder>;
    auto constexpr kQuadPts  = QuadratureRuleType::kPoints;
    auto constexpr kQuadratureOrder = QuadratureOrder;
    auto const numberOfElements     = mesh.E.cols();
    bool const bHasDeterminants = (detJe.rows() == kQuadPts) and (detJe.cols() == numberOfElements);
    if (not bHasDeterminants)
    {
        std::string const what = fmt::format(
            "Expected element jacobian determinants of dimensions {}x{} for element quadrature of "
            "order={}, but got {}x{}",
            kQuadPts,
            numberOfElements,
            kQuadratureOrder,
            detJe.rows(),
            detJe.cols());
        throw std::invalid_argument(what);
    }
    // Precompute element shape functions
    auto constexpr kNodesPerElement             = ElementType::kNodes;
    Matrix<kNodesPerElement, kQuadPts> const Ng = ShapeFunctions<ElementType, kQuadratureOrder>();
    // Integrate shape functions
    MatrixX N     = MatrixX::Zero(kNodesPerElement, numberOfElements);
    auto const wg = common::ToEigen(QuadratureRuleType::weights);
    tbb::parallel_for(Index{0}, Index{numberOfElements}, [&](Index e) {
        for (auto g = 0; g < QuadratureRuleType::kPoints; ++g)
        {
            N.col(e) += (wg(g) * detJe(g, e)) * Ng.col(g);
        }
    });
    return N;
}

/**
 * @brief Computes gradients of FEM basis functions in reference element.
 *
 * Only works for linear maps, but we do not emit an error when `TElement::bHasConstantJacobian` is
 * `false`, since the element's function space might be non-linear, while its current configuration
 * induces a linear map. I.e., a \f$ k^\text{th} \f$ order element that is purely a rigid
 transformation on the
 * reference element still induces a linear map, even if the element's function space is \f$
 k^\text{th} \f$ order.
 * It is up to the user to give the right inputs, and we cannot/won't check those.
 *
 * @note
 * Since \f$ \phi(X) = N(\xi(X)) \f$, to be mathematically precise, what we should compute is:
 * \f{eqnarray*}{
 * \nabla \phi(X) &= \nabla \xi N(\xi(X)) J_X \xi(X) \\
 *                &= \nabla N * J_X \xi(X)
 * \f}
 * This requires the Jacobian of the inverse map taking domain element to reference element.
 * Because this map is potentially non-linear, we compute it via Gauss-Newton iterations in
 * Jacobian.h. Hence, to get the jacobian of that map, we also need to compute derivatives of
 * the Gauss-Newton iterations in Jacobian.h.
 * @note
 * However, we assume that domain elements are linear transformations of reference elements,
 * so that the inverse map is linear, i.e. the Jacobian is constant. Hence,
 * \f{eqnarray*}{
 * X(\xi) &= X * N(\xi) \\
 * J &= X * \nabla_\xi N \\
 * X(\xi) &= X_0 + J*\xi \\
 * J \xi &= X - X_0
 * \f}
 * @note
 * If J is square, then
 * \f{eqnarray*}{
 * \xi &= J^{-1} (X - X_0) \\
 * \nabla_X N(\xi(X)) &= \nabla_\xi N * J^{-1} \\
 * \nabla_X \phi^T  &= J^{-T} \left[ \nabla_\xi N \right]^T
 * \f}
 * @note
 * If J is rectangular, then
 * \f{eqnarray*}{
 * (J^T J) \xi      &= J^T (X - X_0) \\
 * \xi              &= (J^T J)^{-1} J^T (X - X_0) \\
 * \nabla_X N(\xi(X)) &= \nabla_\xi N * (J^T J)^{-1} J^T \\
 * \left[ \nabla_X \phi \right]^T  &= J (J^T J)^{-1} \left[ \nabla_\xi N \right]^T
 * \f}
 * @note
 * For non-linear elements, like hexahedra or quadrilaterals, the accuracy of the gradients
 * might be unacceptable, but will be exact, if domain hex or quad elements are linear
 * transformations on reference hex/quad elements. This is the case for axis-aligned elements,
 * for example, which would arise when constructing a mesh from an octree or quadtree.
 *
 * @tparam TDerivedXi Eigen dense expression type for reference position
 * @tparam TDerivedX Eigen dense expression type for element nodal positions
 * @tparam TElement Element type
 * @param Xi Point in reference element at which to evaluate the gradients
 * @param X Element vertices, i.e. nodes of affine element
 * @return `|# nodes|x|Dims|` matrix of basis function gradients in rows
 */
template <CElement TElement, class TDerivedXi, class TDerivedX>
auto ShapeFunctionGradients(
    Eigen::MatrixBase<TDerivedXi> const& Xi,
    Eigen::MatrixBase<TDerivedX> const& X) -> Matrix<TElement::kNodes, TDerivedX::RowsAtCompileTime>
{
    auto constexpr kInputDims           = TElement::kDims;
    auto constexpr kOutputDims          = TDerivedX::RowsAtCompileTime;
    auto constexpr kNodes               = TElement::kNodes;
    using AffineElementType             = typename TElement::AffineBaseType;
    Matrix<kNodes, kInputDims> const GN = TElement::GradN(Xi);
    Matrix<kOutputDims, kInputDims> J;
    Matrix<kNodes, kOutputDims> GP;

    bool constexpr bIsElementLinear = std::is_same_v<TElement, AffineElementType>;
    if constexpr (bIsElementLinear)
    {
        J = X * GN;
    }
    else
    {
        J = X * AffineElementType::GradN(Xi);
    }
    bool constexpr bIsJacobianSquare = kInputDims == kOutputDims;
    if (bIsJacobianSquare)
    {
        GP.transpose() = J.transpose().fullPivLu().solve(GN.transpose());
    }
    else
    {
        Matrix<kInputDims, kInputDims> const JTJ = J.transpose() * J;
        GP.transpose()                           = J * JTJ.ldlt().solve(GN.transpose());
    }
    return GP;
}

/**
 * @brief Computes nodal shape function gradients at each element quadrature points
 * @tparam Order Quadrature order
 * @tparam TMesh Mesh type
 * @param mesh FEM mesh
 * @return `|# element nodes| x |# dims * # quad.pts. * # elements|` matrix of shape function
 * gradients
 */
template <int QuadratureOrder, CMesh TMesh>
MatrixX ShapeFunctionGradients(TMesh const& mesh)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionGradients");
    using MeshType              = TMesh;
    using ElementType           = typename MeshType::ElementType;
    using QuadratureRuleType    = typename ElementType::template QuadratureType<QuadratureOrder>;
    using AffineElementType     = typename ElementType::AffineBaseType;
    auto const numberOfElements = mesh.E.cols();
    auto constexpr kNodesPerElement = ElementType::kNodes;
    auto const Xg                   = common::ToEigen(QuadratureRuleType::points)
                        .reshaped(QuadratureRuleType::kDims + 1, QuadratureRuleType::kPoints)
                        .template bottomRows<ElementType::kDims>();
    MatrixX GNe(kNodesPerElement, numberOfElements * MeshType::kDims * QuadratureRuleType::kPoints);
    tbb::parallel_for(Index{0}, Index{numberOfElements}, [&](Index e) {
        auto const nodes                = mesh.E.col(e);
        auto const vertices             = nodes(ElementType::Vertices);
        auto constexpr kRowsJ           = MeshType::kDims;
        auto constexpr kColsJ           = AffineElementType::kNodes;
        Matrix<kRowsJ, kColsJ> const Ve = mesh.X(Eigen::placeholders::all, vertices);
        for (auto g = 0; g < QuadratureRuleType::kPoints; ++g)
        {
            auto const GP          = ShapeFunctionGradients<ElementType>(Xg.col(g), Ve);
            auto constexpr kStride = MeshType::kDims * QuadratureRuleType::kPoints;
            GNe.block<kNodesPerElement, MeshType::kDims>(0, e * kStride + g * MeshType::kDims) = GP;
        }
    });
    return GNe;
}

/**
 * @brief Computes nodal shape function gradients at evaluation points Xg.
 * @tparam TDerivedE Eigen dense expression type for element indices
 * @tparam TDerivedXg Eigen dense expression type for evaluation points
 * @tparam TMesh Mesh type
 * @param mesh FEM mesh
 * @param E `|# eval.pts.|` array of elements associated with evaluation points
 * @param Xg `|# dims|x|# eval.pts.|` evaluation points
 * @param bXgInReferenceSpace Consider evaluation points Xg as having coordinates in reference space
 * @return `|# element nodes| x |E.size() * mesh.dims|` nodal shape function gradients at evaluation
 * points Xg
 */
template <CMesh TMesh, class TDerivedE, class TDerivedXg>
MatrixX ShapeFunctionGradientsAt(
    TMesh const& mesh,
    Eigen::DenseBase<TDerivedE> const& E,
    Eigen::MatrixBase<TDerivedXg> const& Xg,
    bool bXgInReferenceSpace = false)
{
    PBAT_PROFILE_NAMED_SCOPE("pbat.fem.ShapeFunctionGradientsAt");
    using MeshType          = TMesh;
    using ElementType       = typename MeshType::ElementType;
    using AffineElementType = typename ElementType::AffineBaseType;

    MatrixX Xi{};
    if (not bXgInReferenceSpace)
    {
        // NOTE: Should expose the "iterations" parameter to parent function
        Xi = ReferencePositions(mesh, E, Xg);
    }
    auto const numberOfEvaluationPoints = Xg.cols();
    auto constexpr kNodesPerElement     = ElementType::kNodes;
    MatrixX GNe(kNodesPerElement, numberOfEvaluationPoints * MeshType::kDims);
    tbb::parallel_for(Index{0}, Index{numberOfEvaluationPoints}, [&](Index g) {
        auto const e                    = E(g);
        auto const nodes                = mesh.E.col(e);
        auto const vertices             = nodes(ElementType::Vertices);
        auto constexpr kRowsJ           = MeshType::kDims;
        auto constexpr kColsJ           = AffineElementType::kNodes;
        Matrix<kRowsJ, kColsJ> const Ve = mesh.X(Eigen::placeholders::all, vertices);
        auto const GP = bXgInReferenceSpace ? ShapeFunctionGradients<ElementType>(Xg.col(g), Ve) :
                                              ShapeFunctionGradients<ElementType>(Xi.col(g), Ve);
        GNe.block<kNodesPerElement, MeshType::kDims>(0, g * MeshType::kDims) = GP;
    });
    return GNe;
}

} // namespace fem
} // namespace pbat

#endif // PBAT_FEM_SHAPE_FUNCTIONS_H
