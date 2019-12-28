#include "Conversion/StencilToStandard/ConvertStencilToStandard.h"
#include "Dialect/Stencil/Passes.h"
#include "Dialect/Stencil/StencilDialect.h"
#include "Dialect/Stencil/StencilOps.h"
#include "Dialect/Stencil/StencilTypes.h"
#include "mlir/Dialect/AffineOps/AffineOps.h"
#include "mlir/Dialect/StandardOps/Ops.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/LLVM.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <bits/stdint-intn.h>
#include <cstddef>
//#include <bits/stdint-intn.h>

using namespace mlir;

namespace {

// Helper method getting the parent loop nest
SmallVector<AffineForOp, 3> getLoopNest(Operation *operation) {
  SmallVector<AffineForOp, 3> result;
  Operation *current = operation;
  while (AffineForOp loop = current->getParentOfType<AffineForOp>()) {
    current = loop.getOperation();
    result.push_back(loop);
  }
  return result;
}

// Helper method computing a memref type
MemRefType getMemRefType(ArrayRef<int64_t> lb, ArrayRef<int64_t> ub,
                         Type elementType, MLIRContext *context) {
  assert(lb.size() == ub.size() && "expected bounds to have the same size");
  assert(lb.size() >= 1 && "expected bounds to have at least one dimension");

  // Compute the array shape
  SmallVector<int64_t, 3> shape(lb.size());
  llvm::transform(llvm::zip(ub, lb), shape.begin(),
                  [](std::tuple<int64_t, int64_t> x) {
                    return std::get<0>(x) - std::get<1>(x);
                  });

  // Compute strides
  SmallVector<int64_t, 3> strides(lb.size());
  strides[0] = 1;
  for (size_t i = 1, e = strides.size(); i != e; ++i)
    strides[i] = strides[i - 1] * shape[i - 1];

  // Compute offset
  int64_t offset = 0;
  for (size_t i = 0, e = strides.size(); i != e; ++i)
    offset += strides[i] * -lb[i];

  // Compute an index map that indexes starting from the origin
  AffineMap indexMap = makeStridedLinearLayoutMap(strides, offset, context);
  return MemRefType::get(shape, elementType, indexMap, 0);
}

//===----------------------------------------------------------------------===//
// Rewriting Pattern
//===----------------------------------------------------------------------===//

class FuncOpLowering : public ConversionPattern {
public:
  explicit FuncOpLowering(MLIRContext *context)
      : ConversionPattern(FuncOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto funcOp = cast<FuncOp>(operation);

    // Convert the input types
    SmallVector<Type, 10> inputTypes;
    for (auto argument : funcOp.getArguments()) {
      Type argType = argument->getType();
      // Verify no view types
      if (argType.getKind() == stencil::StencilTypes::View) {
        operation->emitError("unexpected argument type '") << argType << "'";
        return matchFailure();
      }

      // Compute the input types of the converted stencil program
      if (argType.getKind() == stencil::StencilTypes::Field) {
        Type inputType = NoneType();
        for (auto &use : argument->getUses()) {
          if (auto assertOp = dyn_cast<stencil::AssertOp>(use.getOwner())) {
            Type elementType =
                argType.cast<stencil::FieldType>().getElementType();
            inputType = getMemRefType(assertOp.getLB(), assertOp.getUB(),
                                      elementType, assertOp.getContext());
            break;
          }
        }
        if (inputType == NoneType()) {
          operation->emitError("failed to find stencil assert for input field");
          return matchFailure();
        }
        inputTypes.push_back(inputType);
      } else {
        inputTypes.push_back(argType);
      }
    }
    if (funcOp.getNumResults() > 0) {
      operation->emitError("expected stencil programs return void");
      return matchFailure();
    }

    // Compute replacement function
    auto replacementType = rewriter.getFunctionType(inputTypes, {});
    auto replacementSymbol =
        funcOp.getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName());
    auto replacementOp = rewriter.create<FuncOp>(
        loc, replacementSymbol.getValue(), replacementType, llvm::None);

    // Replace the function body
    Block *entryBlock = replacementOp.addEntryBlock();
    for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i)
      funcOp.getArgument(i)->replaceAllUsesWith(entryBlock->getArgument(i));
    auto &operations =
        funcOp.getOperation()->getRegion(0).front().getOperations();
    entryBlock->getOperations().splice(entryBlock->begin(), operations);

    // Erase the original function op
    rewriter.eraseOp(operation);

    return matchSuccess();
  }
};

class AssertOpLowering : public ConversionPattern {
public:
  explicit AssertOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::AssertOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    rewriter.eraseOp(operation);
    return matchSuccess();
  }
};

class LoadOpLowering : public ConversionPattern {
public:
  explicit LoadOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::LoadOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    // Remove the operation and replace the result with the field operand
    operation->getOpResult(0).replaceAllUsesWith(
        operation->getOpOperand(0).get());
    rewriter.eraseOp(operation);
    return matchSuccess();
  }
};

class ReturnOpLowering : public ConversionPattern {
public:
  explicit ReturnOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::ReturnOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto returnOp = cast<stencil::ReturnOp>(operation);

    // Get the affine loops
    SmallVector<AffineForOp, 3> loops = getLoopNest(operation);
    SmallVector<Value, 3> loopIVs(loops.size(), nullptr);
    llvm::transform(loops, loopIVs.begin(), [](AffineForOp affineForOp) {
      return affineForOp.getInductionVar();
    });
    if (loops.empty())
      return matchFailure();

    // Get temporary buffers
    SmallVector<Operation *, 10> allocOps;
    Operation *currentOp = loops.back().getOperation();
    for (unsigned i = 0, e = returnOp.getNumOperands(); i != e; ++i) {
      currentOp = currentOp->getPrevNode();
      allocOps.push_back(currentOp);
      assert(dyn_cast<AllocOp>(currentOp) &&
             "failed to find allocation for results");
    }
    SmallVector<Value, 10> allocVals(allocOps.size());
    llvm::transform(llvm::reverse(allocOps), allocVals.begin(),
                    [](Operation *allocOp) { return allocOp->getResult(0); });

    // Replace the return op by store ops
    for (unsigned i = 0, e = returnOp.getNumOperands(); i != e; ++i)
      rewriter.create<AffineStoreOp>(loc, returnOp.getOperand(i), allocVals[i],
                                     loopIVs);
    rewriter.eraseOp(operation);

    return matchSuccess();
  }
};

class ApplyOpLowering : public ConversionPattern {
public:
  explicit ApplyOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::ApplyOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto applyOp = cast<stencil::ApplyOp>(operation);

    // Get the loop bounds of the apply op
    assert(applyOp.lb().hasValue() && applyOp.ub().hasValue() &&
           "expected apply to have valid bounds");
    auto lb = applyOp.getLB();
    auto ub = applyOp.getUB();

    // Allocate and deallocate storage for every output
    for (unsigned i = 0, e = applyOp.getNumResults(); i != e; ++i) {
      auto Type = applyOp.getResult(i)->getType();
      auto elementType = Type.cast<stencil::ViewType>().getElementType();
      auto allocOp = rewriter.create<AllocOp>(
          loc, getMemRefType(lb, ub, elementType, applyOp.getContext()));
      applyOp.getResult(i)->replaceAllUsesWith(allocOp.getResult());
      auto returnOp = allocOp.getParentRegion()->back().getTerminator();
      rewriter.setInsertionPoint(returnOp);
      rewriter.create<DeallocOp>(loc, allocOp.getResult());
      rewriter.setInsertionPointAfter(allocOp);
    }

    // Generate the apply loop nest
    assert(lb.size() == ub.size() && "expected bounds to have the same size");
    assert(lb.size() >= 1 && "expected bounds to at least one dimension");
    AffineForOp loop;
    for (size_t i = 0, e = lb.size(); i != e; ++i) {
      loop = rewriter.create<AffineForOp>(loc, lb.rbegin()[i], ub.rbegin()[i]);
      rewriter.setInsertionPointToStart(loop.getBody());
    }

    // Forward the apply operands and copy the body
    for (size_t i = 0, e = applyOp.operands().size(); i < e; ++i) {
      applyOp.getBody()->getArgument(i)->replaceAllUsesWith(
          applyOp.getOperand(i));
    }
    Block *entryBlock = loop.getBody();
    auto &operations = applyOp.getBody()->getOperations();
    entryBlock->getOperations().splice(entryBlock->begin(), operations);
    rewriter.eraseOp(operation);

    return matchSuccess();
  }
};

class AccessOpLowering : public ConversionPattern {
public:
  explicit AccessOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::AccessOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto accessOp = cast<stencil::AccessOp>(operation);

    // Get the affine loops
    SmallVector<AffineForOp, 3> loops = getLoopNest(operation);
    SmallVector<Value, 3> loopIVs(loops.size());
    llvm::transform(loops, loopIVs.begin(), [](AffineForOp affineForOp) {
      return affineForOp.getInductionVar();
    });
    if (loops.empty())
      return matchFailure();
    assert(loops.size() == accessOp.getOffset().size() &&
           "expected loop nest and access offset to have the same size");

    // Compute the access offsets
    auto addExpr = rewriter.getAffineDimExpr(0) + rewriter.getAffineDimExpr(1);
    auto addMap = AffineMap::get(2, 0, addExpr);
    auto accessOffset = accessOp.getOffset();
    SmallVector<Value, 3> loadOffset;
    for (size_t i = 0, e = accessOffset.size(); i != e; ++i) {
      auto constantOp = rewriter.create<ConstantIndexOp>(loc, accessOffset[i]);
      ValueRange addParams = {loopIVs[i], constantOp.getResult()};
      auto affineApplyOp =
          rewriter.create<AffineApplyOp>(loc, addMap, addParams);
      loadOffset.push_back(affineApplyOp.getResult());
    }

    // Replace the access op by a load op
    rewriter.replaceOpWithNewOp<AffineLoadOp>(operation, accessOp.view(),
                                              loadOffset);

    return matchSuccess();
  }
};

class StoreOpLowering : public ConversionPattern {
public:
  explicit StoreOpLowering(MLIRContext *context)
      : ConversionPattern(stencil::StoreOp::getOperationName(), 1, context) {}

  PatternMatchResult
  matchAndRewrite(Operation *operation, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const override {
    auto loc = operation->getLoc();
    auto storeOp = cast<stencil::StoreOp>(operation);

    // Remove allocation and deallocation
    rewriter.eraseOp(storeOp.view()->getDefiningOp());
    for (auto &use : storeOp.view()->getUses()) {
      if (auto deallocOp = dyn_cast<DeallocOp>(use.getOwner()))
        rewriter.eraseOp(deallocOp);
    }

    // Replace all uses of the temporary storage by the output storage
    storeOp.view()->replaceAllUsesWith(storeOp.field());
    rewriter.eraseOp(operation);

    return matchSuccess();
  }
};

//===----------------------------------------------------------------------===//
// Conversion Target
//===----------------------------------------------------------------------===//

class StencilToStandardTarget : public ConversionTarget {
public:
  explicit StencilToStandardTarget(MLIRContext &context)
      : ConversionTarget(context) {}

  bool isDynamicallyLegal(Operation *op) const override {
    if (auto funcOp = dyn_cast<FuncOp>(op)) {
      return !funcOp.getAttr(
                 stencil::StencilDialect::getStencilProgramAttrName()) &&
             !funcOp.getAttr(
                 stencil::StencilDialect::getStencilFunctionAttrName());
    } else
      return true;
  }
};

//===----------------------------------------------------------------------===//
// Rewriting Pass
//===----------------------------------------------------------------------===//

struct StencilToStandardPass : public ModulePass<StencilToStandardPass> {
  void runOnModule() override;
};

void StencilToStandardPass::runOnModule() {
  OwningRewritePatternList patterns;
  auto module = getModule();

  populateStencilToStandardConversionPatterns(patterns, module.getContext());

  StencilToStandardTarget target(*(module.getContext()));
  target.addLegalDialect<AffineOpsDialect>();
  target.addLegalDialect<StandardOpsDialect>();
  target.addDynamicallyLegalOp<FuncOp>();

  if (failed(applyPartialConversion(module, target, patterns)))
    signalPassFailure();
}

} // namespace

void mlir::populateStencilToStandardConversionPatterns(
    mlir::OwningRewritePatternList &patterns, mlir::MLIRContext *ctx) {
  patterns
      .insert<FuncOpLowering, AssertOpLowering, LoadOpLowering, ApplyOpLowering,
              AccessOpLowering, StoreOpLowering, ReturnOpLowering>(ctx);
}

std::unique_ptr<OpPassBase<ModuleOp>>
mlir::stencil::createConvertStencilToStandardPass() {
  return std::make_unique<StencilToStandardPass>();
}

static PassRegistration<StencilToStandardPass>
    pass("convert-stencil-to-standard",
         "Convert stencil dialect to standard operations");
