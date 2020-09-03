#ifndef DIALECT_STENCIL_STENCILOPS_H
#define DIALECT_STENCIL_STENCILOPS_H

#include "Dialect/Stencil/StencilTypes.h"
#include "Dialect/Stencil/StencilUtils.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "mlir/Support/LogicalResult.h"
#include <cstdint>
#include <numeric>

namespace mlir {
namespace stencil {

#include "Dialect/Stencil/StencilOpsInterfaces.h.inc"

/// Retrieve the class declarations generated by TableGen
#define GET_OP_CLASSES
#include "Dialect/Stencil/StencilOps.h.inc"

// Base class for the stencil apply op canonicalization
struct ApplyOpPattern : public OpRewritePattern<stencil::ApplyOp> {
  ApplyOpPattern(MLIRContext *context);

  stencil::ApplyOp cleanupOpArguments(stencil::ApplyOp applyOp,
                                      PatternRewriter &rewriter) const;

  LogicalResult cleanupOpResults(stencil::ApplyOp applyOp,
                                 PatternRewriter &rewriter) const;
};

} // namespace stencil
} // namespace mlir

#endif // DIALECT_STENCIL_STENCILOPS_H
