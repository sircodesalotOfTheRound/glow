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

#ifndef GLOW_TORCH_GLOW_SRC_TRAINING_TORCHGLOWTRAINING_H
#define GLOW_TORCH_GLOW_SRC_TRAINING_TORCHGLOWTRAINING_H

#include "PyTorchCommon.h"
#include "glow/ExecutionEngine/ExecutionEngine.h"
#include "glow/Graph/Graph.h"
#include "llvm/Support/Error.h"
#include <torch/csrc/jit/ir.h>

namespace glow {

/// Loads and trains Glow models from PyTorch/ONNX.
class TorchGlowTraining {
public:
  /// Exporter parameters.
  struct ONNXWriterParameters {
    size_t irVersion{3};
    size_t opsetVersion{10};
  };

  /// Explains how to prepare the input model for training.
  enum class RandomizeWeights {
    // Detects mode automatically depending on file extension.
    // PyTorch models trigger the weights randomization YES,
    // ONNX models don't -> NO.
    AUTO = 0,
    YES = 1,
    NO = 2,
  };

private:
  ExecutionEngine engine_;
  PlaceholderBindings bindings_;
  Function *F_{nullptr};
  Function *TF_{nullptr};
  std::vector<glow::Placeholder *> inputPHs_;
  std::vector<glow::Placeholder *> outputPHs_;
  Placeholder *selectedPH_{nullptr};
  ONNXWriterParameters parameters_;

  /// Releases internal resources.
  void clear();

public:
  /// Construct TorchGlowTraining object.
  TorchGlowTraining() = default;

  /// Cleans up the internals.
  ~TorchGlowTraining();

  /// Public interface, methods must be called in the strict order, i.e.
  /// once init() -> repeatedly train() -> repeatedly save().

  /// Initializes internal Glow objects from \p modelFile file, uses provided
  /// \p backend name, ONNX exporter \p parameters, \p inputs, \p settings,
  /// and training configuration \p config for training algorithm, randomizes
  /// weights according to the provided \p mode.
  /// \returns error on failure.
  llvm::Error
  init(llvm::StringRef modelFile, std::vector<torch::jit::IValue> &inputs,
       llvm::StringRef backend, const ONNXWriterParameters &parameters,
       const PyTorchLoaderSettings &settings, const TrainingConfig &config,
       RandomizeWeights mode = RandomizeWeights::AUTO);

  /// Trains the loaded model from the provided \p samples and \p labels.
  /// Samples and labels must have the compatible dimensions and types.
  /// Caller can provide one or more samples and correspondently labels.
  /// Method can be invoked as many times as required.
  /// \returns error in case of uninitiated model or invalid input parameters.
  llvm::Error train(const Tensor &samples, const Tensor &labels);

  /// Saves the trained model in ONNX (extended) format to the provided
  /// \p snapshotFile. It's safe to call this method any time after train()
  /// calls. Method leaves the internal trained weights unaffected, and caller
  /// can continue to call train() method again.
  /// \returns error on failure.
  llvm::Error save(llvm::StringRef snapshotFile);
};

} // namespace glow

#endif // GLOW_TORCH_GLOW_SRC_TRAINING_TORCHGLOWTRAINING_H
