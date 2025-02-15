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
#ifndef GLOW_PARTITIONER_PARTITIONERBASE_H
#define GLOW_PARTITIONER_PARTITIONERBASE_H

#include "glow/Partitioner/PartitionerTypes.h"
#include "glow/Support/Error.h"

namespace glow {

using namespace runtime;
/// Given a module, partitions each of the its functions into multiple ones
/// based on memory constraints and minimizes the communication cost.
class PartitionerBase {
public:
  virtual ~PartitionerBase() = default;

  /// Decompose each function in a module. \p cctx is used in function
  /// optimization. \returns the partition result.
  virtual llvm::Expected<DAGListTy> partition(CompilationContext &cctx) = 0;

  /// Dump the partition result \p partitions to a dot file with name \p
  /// dotFilename. Since now all functions belong to a function family and they
  /// have the same partition, we only dump the one function's partition.
  void dumpDAG(llvm::StringRef dotFilename, const DAGListTy &partitions) const;

protected:
  /// Given the node-function mapping \p mapping, do the actual partitioning. If
  /// \p saveDAG is true, the DAG will be generated. \returns the final
  /// partitions or an empty partition (If \p saveDAG is false).
  DAGListTy doPartitioning(llvm::StringRef funcName, std::vector<Function *>,
                           Module *module, NodeToFunctionMap &mapping,
                           bool saveDAG);
};
} // namespace glow
#endif // GLOW_PARTITIONER_PARTITIONER_H
