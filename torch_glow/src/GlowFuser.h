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

#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/passes/graph_fuser.h>
#include <torch/csrc/jit/passes/utils/subgraph_utils.h>

namespace glow {

typedef std::function<bool(torch::jit::Node *)> isSupportFunc;

/// Performs specific fusion for Linear operator.
void FuseLinear(std::shared_ptr<torch::jit::Graph> &graph);
void GlowCustomFuse(std::shared_ptr<torch::jit::Graph> graph, isSupportFunc fn,
                    at::Symbol kind);
} // namespace glow
