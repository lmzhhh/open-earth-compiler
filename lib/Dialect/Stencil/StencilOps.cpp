#include "Dialect/Stencil/StencilOps.h"
#include "Dialect/Stencil/StencilDialect.h"
#include "Dialect/Stencil/StencilTypes.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Block.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/Region.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/Functional.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/ArrayRef.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// stencil.access
//===----------------------------------------------------------------------===//

void stencil::AccessOp::build(Builder *builder, OperationState &state,
                              Value *view, ArrayRef<int64_t> offset) {
  // Make sure that the offset has the right size
  assert(offset.size() == 3 && "expected offset with 3 elements");

  // Extract the element type of the view.
  Type elementType = view->getType().cast<stencil::ViewType>().getElementType();

  // Add an SSA argument
  state.addOperands(view);
  // Add the offset attribute
  state.addAttribute(getOffsetAttrName(), builder->getI64ArrayAttr(offset));
  // Set the return type
  state.addTypes(elementType);
}

static ParseResult parseAccessOp(OpAsmParser &parser, OperationState &state) {
  FunctionType funcType;
  ArrayAttr offset;
  SmallVector<OpAsmParser::OperandType, 1> operands;

  // Parse the view
  if (parser.parseOperandList(operands) ||
      parser.parseAttribute(offset, stencil::AccessOp::getOffsetAttrName(),
                            state.attributes))
    return failure();
  // Make sure it has the right number of dimensions
  if (offset.size() != 3) {
    parser.emitError(parser.getCurrentLocation(),
                     "expected offset to have three components");
    return failure();
  }

  // Parse optional attributes as well as the view type
  if (parser.parseOptionalAttrDict(state.attributes) ||
      parser.parseColonType<FunctionType>(funcType) ||
      parser.resolveOperands(operands, funcType.getInputs(),
                             parser.getCurrentLocation(), state.operands))
    return failure();

  // Add the return value
  if (parser.addTypesToList(funcType.getResults(), state.types))
    return failure();

  return success();
}

static void print(stencil::AccessOp accessOp, OpAsmPrinter &printer) {
  // Use the TableGen'd accessors to operands
  Value *view = accessOp.view();
  Attribute offset = accessOp.offset();

  printer << stencil::AccessOp::getOperationName() << ' ' << *view;
  printer.printAttribute(offset);
  printer.printOptionalAttrDict(accessOp.getAttrs(), /*elidedAttrs=*/{
                                    stencil::AccessOp::getOffsetAttrName()});
  printer << " : (";
  printer.printType(view->getType());
  printer << ") -> ";
  printer.printType(accessOp.getResult()->getType());
}

static LogicalResult verify(stencil::AccessOp accessOp) {
  stencil::ViewType viewType =
      accessOp.view()->getType().cast<stencil::ViewType>();
  Type elementType = viewType.getElementType();
  Type resultType = accessOp.getResult()->getType();

  if (resultType != elementType)
    return accessOp.emitOpError("inconsistent result type '")
           << resultType << "' and element type '" << elementType << "'";

  return success();
}

//===----------------------------------------------------------------------===//
// stencil.load
//===----------------------------------------------------------------------===//

void stencil::LoadOp::build(Builder *builder, OperationState &state,
                            Value *field) {
  Type elementType =
      field->getType().cast<stencil::FieldType>().getElementType();
  StencilStorage::Allocation allocation =
      field->getType().cast<stencil::FieldType>().getAllocation();

  state.addOperands(field);
  state.addTypes(
      stencil::ViewType::get(builder->getContext(), elementType, allocation));
}

static ParseResult parseLoadOp(OpAsmParser &parser, OperationState &state) {
  FunctionType funcType;
  SmallVector<OpAsmParser::OperandType, 1> operands;

  if (parser.parseOperandList(operands) ||
      parser.parseOptionalAttrDict(state.attributes) ||
      parser.parseColonType<FunctionType>(funcType) ||
      parser.resolveOperands(operands, funcType.getInputs(),
                             parser.getCurrentLocation(), state.operands) ||
      parser.addTypesToList(funcType.getResults(), state.types))
    return failure();

  return success();
}

static void print(stencil::LoadOp loadOp, OpAsmPrinter &printer) {
  Value *field = loadOp.field();

  Type fieldType = field->getType();
  Type viewType = loadOp.res()->getType();

  printer << stencil::LoadOp::getOperationName() << ' ' << *field;
  printer.printOptionalAttrDict(loadOp.getAttrs(), /*elidedAttrs=*/{});
  printer << " : (";
  printer.printType(fieldType);
  printer << ") -> ";
  printer.printType(viewType);
}

static LogicalResult verify(stencil::LoadOp loadOp) {
  stencil::FieldType fieldType =
      loadOp.field()->getType().cast<stencil::FieldType>();
  stencil::ViewType viewType =
      loadOp.res()->getType().cast<stencil::ViewType>();

  Type fieldElementType = fieldType.getElementType();
  Type viewElementType = viewType.getElementType();
  if (fieldElementType != viewElementType)
    return loadOp.emitOpError("inconsistent field element type '")
           << fieldElementType << "' and view element type '" << viewElementType
           << "'";

  auto fieldAllocation = fieldType.getAllocation();
  auto viewAllocation = viewType.getAllocation();
  if (fieldAllocation != viewAllocation)
    return loadOp.emitOpError("storage allocation is inconsistent");

  return success();
}

//===----------------------------------------------------------------------===//
// stencil.store
//===----------------------------------------------------------------------===//

void stencil::StoreOp::build(Builder *builder, OperationState &state,
                             Value *view, Value *field, ArrayRef<int64_t> lb,
                             ArrayRef<int64_t> ub) {
  // Make sure that the offset has the right size
  assert(lb.size() == 3 && ub.size() == 3 && "expected bounds with 3 elements");

  // Add an SSA arguments
  state.addOperands({view, field});
  // Add the bounds attributes
  state.addAttribute(getLBAttrName(), builder->getI64ArrayAttr(lb));
  state.addAttribute(getUBAttrName(), builder->getI64ArrayAttr(ub));
}

static ParseResult parseStoreOp(OpAsmParser &parser, OperationState &state) {
  OpAsmParser::OperandType view, field;
  ArrayAttr lbAttr, ubAttr;
  Type fieldType, viewType;
  // Parse the store op
  if (parser.parseOperand(view) || parser.parseKeyword("to") ||
      parser.parseOperand(field) ||
      parser.parseAttribute(lbAttr, stencil::StoreOp::getLBAttrName(),
                            state.attributes) ||
      parser.parseAttribute(ubAttr, stencil::StoreOp::getUBAttrName(),
                            state.attributes) ||
      parser.parseOptionalAttrDict(state.attributes) || parser.parseColon() ||
      parser.parseType(viewType) || parser.parseKeyword("to") ||
      parser.parseType(fieldType))
    return failure();

  // Make sure bounds have the right number of dimensions
  if (lbAttr.size() != 3 || ubAttr.size() != 3) {
    parser.emitError(parser.getCurrentLocation(),
                     "expected bounds to have three components");
    return failure();
  }

  if (parser.resolveOperand(view, viewType, state.operands) ||
      parser.resolveOperand(field, fieldType, state.operands))
    return failure();

  return success();
}

static void print(stencil::StoreOp storeOp, OpAsmPrinter &printer) {
  Value *field = storeOp.field();
  Value *view = storeOp.view();
  ArrayAttr lb = storeOp.lb();
  ArrayAttr ub = storeOp.ub();

  printer << stencil::StoreOp::getOperationName() << " " << *view;
  printer << " to " << *field;
  printer.printAttribute(lb);
  printer.printAttribute(ub);
  printer.printOptionalAttrDict(
      storeOp.getAttrs(), /*elidedAttrs=*/{stencil::StoreOp::getLBAttrName(),
                                           stencil::StoreOp::getUBAttrName()});
  printer << " : ";
  printer.printType(view->getType());
  printer << " to ";
  printer.printType(field->getType());
}

static LogicalResult verify(stencil::StoreOp storeOp) {
  stencil::FieldType fieldType = storeOp.getFieldType();
  stencil::ViewType viewType = storeOp.getViewType();

  Type fieldElementType = fieldType.getElementType();
  Type viewElementType = viewType.getElementType();
  if (fieldElementType != viewElementType)
    return storeOp.emitOpError("inconsistent field element type '")
           << fieldElementType << "' and view element type '" << viewElementType
           << "'";

  auto fieldAllocation = fieldType.getAllocation();
  auto viewAllocation = viewType.getAllocation();
  if (fieldAllocation != viewAllocation)
    return storeOp.emitOpError("storage allocation is inconsistent");

  return success();
}

//===----------------------------------------------------------------------===//
// stencil.apply
//===----------------------------------------------------------------------===//

void stencil::ApplyOp::build(Builder *builder, OperationState &result,
                             Block *body, ArrayRef<Value *> operands) {
  result.addOperands(operands);

  // Check the arguments and extract the return types
  for (int i = 0, e = operands.size(); i != e; ++i)
    assert(operands[i]->getType() != body->getArgument(i)->getType() &&
           "expected matching operand and block argument types");
  stencil::ReturnOp returnOp = cast<stencil::ReturnOp>(body->back());
  SmallVector<Type, 12> resultTypes;
  for (auto operandType : returnOp.getOperandTypes()) {
    resultTypes.push_back(stencil::ViewType::get(
        builder->getContext(), operandType, StencilStorage::Allocation::IJK));
  } 

  // Add the body and set the result types
  Region *region = result.addRegion();
  region->push_back(body);
  result.addTypes(resultTypes);
}

static ParseResult parseApplyOp(OpAsmParser &parser, OperationState &state) {
  SmallVector<OpAsmParser::OperandType, 8> operands;
  SmallVector<OpAsmParser::OperandType, 8> arguments;

  // Parse region arguments and the assigned data operands
  llvm::SMLoc loc = parser.getCurrentLocation();
  do {
    OpAsmParser::OperandType currentArgument;
    OpAsmParser::OperandType currentOperand;
    if (parser.parseRegionArgument(currentArgument) || parser.parseEqual() ||
        parser.parseOperand(currentOperand))
      return failure();
    arguments.push_back(currentArgument);
    operands.push_back(currentOperand);
  } while (!parser.parseOptionalComma());

  // Parse optional attributes and the operand types
  SmallVector<Type, 8> operandTypes;
  if (parser.parseOptionalAttrDict(state.attributes) ||
      parser.parseColonTypeList(operandTypes))
    return failure();

  // Parse the body region.
  SmallVector<Type, 8> resultTypes;
  Region *body = state.addRegion();
  if (parser.parseRegion(*body, arguments, operandTypes) ||
      parser.parseColonTypeList(resultTypes) ||
      parser.resolveOperands(operands, operandTypes, loc, state.operands) ||
      parser.addTypesToList(resultTypes, state.types))
    return failure();

  return success();
}

static void print(stencil::ApplyOp applyOp, OpAsmPrinter &printer) {
  printer << stencil::ApplyOp::getOperationName() << ' ';

  // Print the region arguments
  ValueRange operands = applyOp.getOperands();
  if (!applyOp.region().empty() && !operands.empty()) {
    Block *body = applyOp.getBody();
    interleaveComma(llvm::seq<int>(0, operands.size()), printer, [&](int i) {
      printer << *body->getArgument(i) << " = " << *operands[i];
    });
  }

  // Print the optional arguments and the operand types
  printer.printOptionalAttrDict(applyOp.getAttrs());
  printer << " : ";
  interleaveComma(applyOp.getOperandTypes(), printer);

  // Print region and return type
  printer.printRegion(applyOp.region(),
                      /*printEntryBlockArgs=*/false);
  printer << " : ";
  interleaveComma(applyOp.res().getTypes(), printer);
}

static LogicalResult verify(stencil::ApplyOp applyOp) {
  // Check the body takes at least one argument
  auto *body = applyOp.getBody();
  if (body->getNumArguments() == 0)
    return applyOp.emitOpError("expected body to have at least one argument");

  // TODO check the body contains only valid operations

  // Check the number of operands and arguments match
  if (body->getNumArguments() != applyOp.operands().size())
    return applyOp.emitOpError(
        "expected operation and body to have same number of arguments");

  // Check the operands match the block argument types
  for (unsigned i = 0, e = applyOp.operands().size(); i != e; ++i) {
    if (applyOp.getBody()->getArgument(i)->getType() !=
        applyOp.operands()[i]->getType())
      return applyOp.emitOpError(
          "expected operation and body arguments to have the same type");
  }
  return success();
}

//===----------------------------------------------------------------------===//
// stencil.call
//===----------------------------------------------------------------------===//

void stencil::CallOp::build(Builder *builder, OperationState &result,
                            FuncOp callee, stencil::ViewType viewType,
                            ArrayRef<int64_t> offset,
                            ArrayRef<Value *> operands) {
  assert(offset.size() == 3 && "expected offset with 3 elements");
  assert(
      callee.getAttr(stencil::StencilDialect::getStencilFunctionAttrName()) &&
      "only stencil functions can be used in an apply operation");
  assert(callee.getType().getNumResults() == 1 &&
         "expected stencil function to return only one result");
  assert(callee.getType().getResult(0) == viewType.getElementType() &&
         "incompatible stencil function return type "
         "and view type");

  result.addOperands(operands);
  result.addAttribute(getCalleeAttrName(), builder->getSymbolRefAttr(callee));
  result.addAttribute(getOffsetAttrName(), builder->getI64ArrayAttr(offset));
  result.addTypes(viewType);
}

FunctionType stencil::CallOp::getCalleeType() {
  SmallVector<Type, 1> resultTypes({getResult()->getType()});
  SmallVector<Type, 8> argTypes(getOperandTypes());
  return FunctionType::get(argTypes, resultTypes, getContext());
}

static ParseResult parseCallOp(OpAsmParser &parser, OperationState &state) {
  SymbolRefAttr calleeAttr;
  FunctionType funcType;
  SmallVector<OpAsmParser::OperandType, 3> operands;
  ArrayAttr offset;
  auto calleeLoc = parser.getNameLoc();
  if (parser.parseAttribute(calleeAttr, stencil::CallOp::getCalleeAttrName(),
                            state.attributes) ||
      parser.parseOperandList(operands, OpAsmParser::Delimiter::Paren) ||
      parser.parseAttribute(offset, stencil::CallOp::getOffsetAttrName(),
                            state.attributes) ||
      parser.parseOptionalAttrDict(state.attributes) ||
      parser.parseColonType(funcType) ||
      parser.addTypesToList(funcType.getResults(), state.types) ||
      parser.resolveOperands(operands, funcType.getInputs(), calleeLoc,
                             state.operands))
    return failure();

  return success();
}

static void print(stencil::CallOp callOp, OpAsmPrinter &printer) {
  printer << stencil::CallOp::getOperationName() << ' '
          << callOp.getAttr(stencil::CallOp::getCalleeAttrName()) << '(';
  printer.printOperands(callOp.getOperands());
  printer << ')' << callOp.getAttr(stencil::CallOp::getOffsetAttrName());
  printer.printOptionalAttrDict(callOp.getAttrs(),
                                {stencil::CallOp::getCalleeAttrName(),
                                 stencil::CallOp::getOffsetAttrName()});
  printer << " : ";

  FunctionType calleeType = callOp.getCalleeType();
  printer.printType(calleeType);
}

static LogicalResult verify(stencil::CallOp callOp) {
  // Check that the callee attribute was specified.
  auto funAttr =
      callOp.getAttrOfType<SymbolRefAttr>(stencil::CallOp::getCalleeAttrName());
  if (!funAttr)
    return callOp.emitOpError("requires a '")
           << stencil::CallOp::getCalleeAttrName()
           << "' symbol reference attribute";
  auto fun = callOp.getParentOfType<ModuleOp>().lookupSymbol<FuncOp>(
      funAttr.getLeafReference());
  if (!fun)
    return callOp.emitOpError() << "'" << funAttr.getLeafReference()
                                << "' does not reference a valid function";
  if (!fun.getAttr(stencil::StencilDialect::getStencilFunctionAttrName()))
    return callOp.emitOpError() << "'" << funAttr.getLeafReference()
                                << "' does not reference a stencil function";

  // Verify that the operand and result types match the callee.
  auto funType = fun.getType();
  if (funType.getNumInputs() != callOp.getNumOperands())
    return callOp.emitOpError("incorrect number of operands for callee");

  for (unsigned i = 0, e = funType.getNumInputs(); i != e; ++i)
    if (callOp.getOperand(i)->getType() != funType.getInput(i))
      return callOp.emitOpError("operand type mismatch");

  if (funType.getNumResults() != 1)
    return callOp.emitOpError("incorrect number of results for callee");

  return success();
}

//===----------------------------------------------------------------------===//
// stencil.return
//===----------------------------------------------------------------------===//

static ParseResult parseReturnOp(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::OperandType, 2> operands;
  SmallVector<Type, 2> operandTypes;
  llvm::SMLoc loc = parser.getCurrentLocation();
  return failure(
      parser.parseOperandList(operands) ||
      parser.parseColonTypeList(operandTypes) ||
      parser.resolveOperands(operands, operandTypes, loc, result.operands));
}

static void print(stencil::ReturnOp returnOp, OpAsmPrinter &printer) {
  printer << stencil::ReturnOp::getOperationName() << ' ';
  printer << returnOp.getOperands() << " : " << returnOp.getOperandTypes();
}

static LogicalResult verify(stencil::ReturnOp returnOp) {
  auto applyOp = cast<stencil::ApplyOp>(returnOp.getParentOp());

  // The operand number and types must match the apply signature
  const auto &results = applyOp.res();
  if (returnOp.getNumOperands() != results.size())
    return returnOp.emitOpError("has ")
           << returnOp.getNumOperands()
           << " operands, but enclosing function returns " << results.size();

  // The return types must match the element types of the returned views
  for (unsigned i = 0, e = results.size(); i != e; ++i)
    if (returnOp.getOperand(i)->getType() !=
        applyOp.getResultViewType(i).getElementType())
      return returnOp.emitError()
             << "type of return operand " << i << " ("
             << returnOp.getOperand(i)->getType()
             << ") doesn't match function result type ("
             << applyOp.getResultViewType(i).getElementType() << ")";

  return success();
}

namespace mlir {
namespace stencil {
#define GET_OP_CLASSES
#include "Dialect/Stencil/StencilOps.cpp.inc"
} // namespace stencil
} // namespace mlir
