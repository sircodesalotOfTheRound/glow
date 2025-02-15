/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GLOW_QUANTIZATION_BASE_BASE_H
#define GLOW_QUANTIZATION_BASE_BASE_H

#include "glow/Base/Tensor.h"
#include "glow/Base/Traits.h"
#include "glow/Base/Type.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>

namespace glow {

/// Main attributes of a quantized tensor.
/// Scale and Offset allow quantization of a float tensor and dequantization of
/// integer tensor back to float one.
struct TensorQuantizationParams {
  float scale;
  int32_t offset;
};

/// A data structure that represents the 32-bit to 8-bit quantization
/// scaling operation. This data structure represents the transformation:
/// (((input >> pre) * scale) + rtn) >> post + offset.
struct QuantizationTransform32To8 {
  int pre;
  int post;
  int scale;
  int offset;

  /// Initializes the transformation based on the conversion formula (above).
  QuantizationTransform32To8(int pre, int post, int scale, int offset)
      : pre(pre), post(post), scale(scale), offset(offset) {}

  /// \returns the scaled integer.
  int32_t transform(int32_t input) {
    // The operation x >> y is rounded down to negative infinity. To get to
    // round-nearest we add (1 << (shift - 1)) to the value prior to shifting.
    int rtn = (1 << (post - 1));
    return ((((input >> pre) * scale) + rtn) >> post) + offset;
  }
};

/// Tensor quantization parameters for a given node.
struct NodeQuantizationInfo {
  std::string nodeOutputName_;
  TensorQuantizationParams tensorQuantizationParams_;

  NodeQuantizationInfo() = default;
  NodeQuantizationInfo(const std::string &nodeOutputName,
                       const TensorQuantizationParams &tensorQuantizationParams)
      : nodeOutputName_(nodeOutputName),
        tensorQuantizationParams_(tensorQuantizationParams) {}

  float Scale() const { return tensorQuantizationParams_.scale; }
  int32_t Offset() const { return tensorQuantizationParams_.offset; }

  /// Get the full node output name based on the node name and output number.
  /// The following format is used: nodename:outputNumber
  static std::string generateNodeOutputName(const std::string &nodeName,
                                            unsigned outputNumber = 0) {
    return nodeName + ":" + std::to_string(outputNumber);
  }
};

/// Struct containing the output name string and node kind for use in the
/// LoweredInfoMap for keeping track of lowered node info.
struct NodeNameAndKind : public Named, public Kinded {
public:
  NodeNameAndKind(llvm::StringRef name, size_t resNo, Kinded::Kind k)
      : Named(NodeQuantizationInfo::generateNodeOutputName(name, resNo)),
        Kinded(k) {}
};

/// Overload < operator for NodeNameAndKind to allow for usage with std::set.
inline bool operator<(const NodeNameAndKind &x, const NodeNameAndKind &y) {
  return x.getName() < y.getName();
}

/// Overload == operator for NodeNameAndKind to allow for usage with std::set.
inline bool operator==(const NodeNameAndKind &x, const NodeNameAndKind &y) {
  return x.getName() == y.getName();
}

/// Used to keep track of the origin of lowered Nodes via output names as
/// determined by NodeQuantizationInfo::generateNodeOutputName(). For example if
/// some NodeValue X is lowered from some NodeValue Y, then the output name of X
/// is a key which maps to a set of names which contains the output name of Y.
using LoweredInfoMap = llvm::StringMap<std::set<NodeNameAndKind>>;

namespace quantization {

enum Schema {
  /// Asymmetric quantization produces ranges not necessarily centered on 0.
  Asymmetric,
  /// Symmetric quantization produces ranges centered on 0.
  Symmetric,
  /// Symmetric quantization produces ranges centered on 0 or -qmin, qmin being
  /// the minimum value of the quantized type.
  /// An offset of qmin (i.e., offset == -128 for int8) represents an unsigned
  /// version of the quantized type with an offset of zero:
  /// For example, int8 is [-128; 127] - (-128) == uint8 [0; 255] - 0
  SymmetricWithUnsigned,
};

/// Configuration for Quantization, passed into \ref quantizeFunction().
struct QuantizationConfiguration {
  /// Infos to use when determining scale and offset for all Nodes inside, and
  /// Placeholders and Constants referenced by, a Function being quantized.
  std::vector<NodeQuantizationInfo> infos{};

  /// Precision to use when quantizing a Function.
  ElemKind precision{ElemKind::Int8QTy};

  /// Schema to use when quantizing a Function.
  quantization::Schema schema{quantization::Schema::Asymmetric};

  /// Whether to use rowwise quantization when quantizing a Function.
  bool enableRowwise{false};

  /// New name for the quantized function. If no name is given then
  /// \ref quantizeFunction() will generate a name.
  std::string newFuncName{""};

  /// If true, the quantizer will abort when encountering a node that it would
  /// like to quantize but the backend cannot support. Note that node kinds in
  /// doNotQuantizeKinds will skip this check and not cause an abort.
  bool assertAllNodesQuantized{false};

  QuantizationConfiguration() = default;
  QuantizationConfiguration(llvm::ArrayRef<NodeQuantizationInfo> i)
      : infos(i) {}
};

/// \returns the value \p in as clipped to the range of \p DestTy.
template <class SrcTy, class DestTy> DestTy clip(SrcTy in) {
  static_assert(sizeof(SrcTy) >= sizeof(DestTy), "Invalid types");

  auto mx = std::numeric_limits<DestTy>::max();
  auto mn = std::numeric_limits<DestTy>::min();
  return std::max<SrcTy>(mn, std::min<SrcTy>(mx, in));
}

/// Converts floating point value to DestTy (quantized type) based on the
/// quantization parameters \p TQP.
template <class DestTy = int8_t>
inline DestTy quantize(float input, const TensorQuantizationParams &TQP) {
  float result = input / TQP.scale + TQP.offset;
  return quantization::clip<int32_t, DestTy>((int32_t)nearbyintf(result));
}

/// Converts a quantized value (type eTy) to floating point based on the
/// quantization parameters \p TQP.
/// Note: use int64_t to cover the 'symmetric int32 with unsigned' case.
template <class eTy = int8_t>
inline float dequantize(eTy input, const TensorQuantizationParams &TQP) {
  return TQP.scale * ((int64_t)input - TQP.offset);
}

/// Converts floating point value to DestTy (quantized type) based on the
/// quantization parameters \p scale and \p offset. If the dest type is int8_t,
/// then an offset of 128 is substracted to convert to int8_t.
template <class DestTy>
inline DestTy quantizeWithFloatOffset(float input, float scale, float offset) {
  uint8_t d = static_cast<uint8_t>((input - offset) / scale);
  if (std::is_same<int8_t, DestTy>::value) {
    d -= 128;
  }
  return static_cast<DestTy>(d);
}

/// Converts a quantized value (type eTy) to floating point based on the
/// quantization parameters \p scale and \p offset. If the input type is int8_t,
/// then an offset of 128 is added to convert to uint8_t.
template <class eTy>
inline float dequantizeWithFloatOffset(eTy input, float scale, float offset) {
  uint8_t d = static_cast<uint8_t>(input);
  if (std::is_same<int8_t, eTy>::value) {
    d += 128;
  }
  return (d * scale) + offset;
}

/// Converts a floating point \p tensor to quantized tensor based on the
/// quantization parameters \p TQP and \p Ty.
Tensor quantizeTensor(const Tensor &tensor, const TensorQuantizationParams &TQP,
                      ElemKind Ty = ElemKind::Int8QTy);

/// Converts quantized tensor \p tensor to floating point tensor of type \p Ty
/// floatKind.
Tensor dequantizeTensor(const Tensor &tensor, ElemKind floatKind);

/// Convert the floating point quantization parameters \p scale and \p offset
/// into the integer sequence of:
/// result = ((input >> pre) * scale) >> post + offset.
/// This scales a 32-bit signed integer word into an 8-bit signed integer.
/// \returns transformation parameters.
QuantizationTransform32To8 quantizeScaleOffset32To8(float scale,
                                                    int32_t offset);

/// Calculate TensorQuantizationParams based on the clipped \p min and \p max
/// floating point range and using the base quantization type \p qTy and the
/// quantization method described by \p schema.
TensorQuantizationParams
chooseQuantizationParams(float min, float max, Schema schema = Asymmetric,
                         ElemKind qTy = ElemKind::Int8QTy);

/// \returns an int8 vector mapping from the \p inTy to the \p outTy given the
/// function \p f.
/// \pre inTy and outTy should be Int8QTy.
std::vector<int8_t> createMapping(TypeRef inTy, TypeRef outTy,
                                  std::function<float(float)> f);

/// Row-wise quantize the tensor \p input. \p scales and \p offsets are
/// generated by each row of \p input, \p output is tensor of the same shape as
/// input, quantized from \p input using \p scales and \p offsets for each
/// row. Note that the shape of input/output can be any non-zero number of
/// dimensions; row refers to all data in the first dimension of the shape.
/// Template parameter \p ScaleT and OffsetT represent the type to use for the
/// scales and offsets for quantization respectively. Template parameter \p QP
/// represents quantization precision, typically int8_t or uint8_t.
template <typename ScaleT, typename OffsetT, typename QP>
void tensorRowwiseQuantization(const Tensor &input, Tensor &output,
                               Tensor &scales, Tensor &offsets,
                               quantization::Schema schema) {
  constexpr bool offsetIsFP = std::is_same<float, OffsetT>::value ||
                              std::is_same<float16_t, OffsetT>::value;
  constexpr bool offsetIsInt32 = std::is_same<int32_t, OffsetT>::value;
  static_assert((offsetIsInt32 && std::is_same<float, ScaleT>::value) ||
                    (offsetIsFP && std::is_same<ScaleT, OffsetT>::value),
                "Invalid combination of Scale/Offset types.");

  const auto fDims = flattenCdr(input.dims());
  Tensor finalIn = input.getUnowned({fDims.first, fDims.second});
  Tensor finalOut = output.getUnowned({fDims.first, fDims.second});
  ShapeHW idim(finalIn.dims());

  auto srcH = finalIn.getHandle<float>();
  auto destH = finalOut.getHandle<QP>();
  auto scalesH = scales.getHandle<ScaleT>();
  auto offsetsH = offsets.getHandle<OffsetT>();
  for (size_t i = 0; i < idim.height; i++) {
    auto slice = srcH.extractSlice(i);
    auto rSrc = slice.getHandle<float>();
    auto res = rSrc.minMaxArg();
    float min = rSrc.raw(res.first);
    float max = rSrc.raw(res.second);
    // Expand the range to include 0.0f so that 0 is exactly representable.
    min = std::min(min, 0.0f);
    max = std::max(max, 0.0f);

    // Handle rowwise quantization for FCs.
    if (offsetIsInt32) {
      TensorQuantizationParams qParams =
          chooseQuantizationParams(min, max, schema);
      for (size_t j = 0; j < idim.width; j++) {
        destH.at({i, j}) = quantization::quantize(srcH.at({i, j}), qParams);
      }
      scalesH.raw(i) = qParams.scale;
      offsetsH.raw(i) = qParams.offset;
    } else if (offsetIsFP) {
      // Handle rowwise quantization for Rowwise quantized SLS.
      float scale = ((double)max - (double)min) / 255.0;
      float offset = min;

      for (size_t j = 0; j < idim.width; j++) {
        destH.at({i, j}) = quantization::quantizeWithFloatOffset<QP>(
            srcH.at({i, j}), scale, offset);
      }
      scalesH.raw(i) = static_cast<ScaleT>(scale);
      offsetsH.raw(i) = static_cast<OffsetT>(offset);
    } else {
      llvm_unreachable("Unsupported offset type.");
    }
  }
}

/// Fused-rowwise quantize the tensor \p input. Scales and offsets are generated
/// from each row of \p input. \p output is tensor of the same shape as input
/// but with extra columns for storing fused scales. Template parameter \p T
/// represents the datatype used for storing the scale and offset in the row.
/// \pre input.dims().size() == 2
/// \pre output.dims().size() == 2
/// \pre input.dims()[1] + 2 * sizeof(T) == output.dims()[1]
template <typename T>
void tensorFusedRowwiseQuantization(const Tensor &input, Tensor &output) {
  // We are fusing the scale and offset onto the end of each row. Thus input and
  // output must both be 2 dimensional, with output having 2*sizeof(T) extra
  // columns for the scale and offset.
  assert(input.dims().size() == 2 && output.dims().size() == 2 &&
         "Input and output must be 2 dimensional.");
  assert(input.dims()[1] + 2 * sizeof(T) == output.dims()[1] &&
         "Output must have 2*sizeof(T) more columns than input.");

  const size_t outWidth = output.dims()[1];
  char *dataBasePtr = output.getUnsafePtr();

  auto srcH = input.getHandle<float>();
  auto destH = output.getHandle<uint8_t>();
  for (size_t i = 0, e = input.dims()[0]; i < e; i++) {
    auto slice = srcH.extractSlice(i);
    auto rSrc = slice.getHandle<float>();
    auto res = rSrc.minMaxArg();
    float min = rSrc.raw(res.first);
    float max = rSrc.raw(res.second);

    min = std::min(min, 0.0f);
    max = std::max(max, 0.0f);

    // This matches the Caffe2 implementation for FloatToRowwiseQuantized8BitsOp
    // found in operators/lengths_reducer_rowwise_8bit_ops.h.
    constexpr float kEqualityThreshold = 1e-10f;
    const float scale = ((max - min) < kEqualityThreshold)
                            ? 1.0
                            : ((double)max - (double)min) / 255.0;
    const float offset = min;

    for (size_t j = 0, f = input.dims()[1]; j < f; j++) {
      destH.at({i, j}) = quantization::quantizeWithFloatOffset<uint8_t>(
          srcH.at({i, j}), scale, offset);
    }

    // Now set the scale/offset at the end of each row.
    T finalScale = static_cast<T>(scale);
    T finalOffset = static_cast<T>(offset);
    char *currRowScaleOffsetPtr =
        dataBasePtr + (i + 1) * outWidth - 2 * sizeof(T);
    memcpy(currRowScaleOffsetPtr, &finalScale, sizeof(T));
    memcpy(currRowScaleOffsetPtr + sizeof(T), &finalOffset, sizeof(T));
  }
}

} // namespace quantization
} // namespace glow

#endif // GLOW_QUANTIZATION_BASE_BASE_H
