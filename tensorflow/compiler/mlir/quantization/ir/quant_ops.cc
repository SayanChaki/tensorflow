/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Quantization Type and Ops Implementation
//
#include "tensorflow/compiler/mlir/quantization/ir/quant_ops.h"

#include <numeric>

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MathExtras.h"
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/quantization/ir/quant_types.h"
#include "tensorflow/compiler/mlir/quantization/ir/type_detail.h"

using namespace mlir;
using namespace mlir::quant;
using namespace mlir::quant::detail;

#include "tensorflow/compiler/mlir/quantization/ir/quant_ops_dialect.cc.inc"

void QuantizationDialect::initialize() {
  addTypes<AnyQuantizedType, CalibratedQuantizedType, UniformQuantizedType,
           UniformQuantizedPerAxisType>();
  addOperations<
#define GET_OP_LIST
#include "tensorflow/compiler/mlir/quantization/ir/quant_ops.cc.inc"
      >();
}

OpFoldResult StorageCastOp::fold(ArrayRef<Attribute> operands) {
  // Matches x -> [scast -> scast] -> y, replacing the second scast with the
  // value of x if the casts invert each other.
  auto srcScastOp = arg().getDefiningOp<StorageCastOp>();
  if (!srcScastOp || srcScastOp.arg().getType() != getType())
    return OpFoldResult();
  return srcScastOp.arg();
}

/// The quantization specification should match the expressed type.
static bool isValidQuantizationSpec(Attribute quantSpec, Type expressed) {
  if (auto typeAttr = quantSpec.dyn_cast<TypeAttr>()) {
    Type spec = typeAttr.getValue();
    if (spec.isa<TensorType, VectorType>())
      return false;

    // The spec should be either a quantized type which is compatible to the
    // expressed type, or a primitive type which is as same as the
    // (element type of) the expressed type.
    if (auto quantizedType = spec.dyn_cast<QuantizedType>())
      return quantizedType.isCompatibleExpressedType(expressed);

    if (auto tensorType = expressed.dyn_cast<TensorType>())
      return spec == tensorType.getElementType();

    if (auto vectorType = expressed.dyn_cast<VectorType>())
      return spec == vectorType.getElementType();
  }
  return false;
}

LogicalResult QuantizeRegionOp::verify() {
  // There are specifications for both inputs and outputs.
  if (getNumOperands() != input_specs().size() ||
      getNumResults() != output_specs().size())
    return emitOpError(
        "has unmatched operands/results number and spec attributes number");

  // Verify that quantization specifications are valid.
  for (auto input : llvm::zip(getOperandTypes(), input_specs())) {
    Type inputType = std::get<0>(input);
    Attribute inputSpec = std::get<1>(input);
    if (!isValidQuantizationSpec(inputSpec, inputType)) {
      return emitOpError() << "has incompatible specification " << inputSpec
                           << " and input type " << inputType;
    }
  }

  for (auto result : llvm::zip(getResultTypes(), output_specs())) {
    Type outputType = std::get<0>(result);
    Attribute outputSpec = std::get<1>(result);
    if (!isValidQuantizationSpec(outputSpec, outputType)) {
      return emitOpError() << "has incompatible specification " << outputSpec
                           << " and output type " << outputType;
    }
  }
  return success();
}

LogicalResult StatisticsOp::verify() {
  auto tensorArg = arg().getType().dyn_cast<TensorType>();
  if (!tensorArg)
    return emitOpError("arg needs to be tensor type.");

  // Verify layerStats attribute.
  {
    auto layerStatsType = layerStats().getType();
    if (!layerStatsType.getElementType().isa<FloatType>()) {
      return emitOpError("layerStats must have a floating point element type");
    }
    if (layerStatsType.getRank() != 1 || layerStatsType.getDimSize(0) != 2) {
      return emitOpError("layerStats must have shape [2]");
    }
  }
  // Verify axisStats (optional) attribute.
  if (axisStats()) {
    if (!axis())
      return emitOpError("axis must be specified for axisStats");

    auto shape = tensorArg.getShape();
    auto argSliceSize =
        std::accumulate(std::next(shape.begin(), *axis()), shape.end(), 1,
                        std::multiplies<int64_t>());

    auto axisStatsType = axisStats()->getType();
    if (!axisStatsType.getElementType().isa<FloatType>()) {
      return emitOpError("axisStats must have a floating point element type");
    }
    if (axisStatsType.getRank() != 2 || axisStatsType.getDimSize(1) != 2 ||
        axisStatsType.getDimSize(0) != argSliceSize) {
      return emitOpError("axisStats must have shape [N,2] "
                         "where N = the slice size defined by the axis dim");
    }
  }
  return success();
}

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/quantization/ir/quant_ops.cc.inc"
