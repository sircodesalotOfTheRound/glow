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

#include "glow/Runtime/HostManager/HostManager.h"
#include "glow/Backends/DeviceManager.h"
#include "glow/Graph/PlaceholderBindings.h"
#include "glow/Optimizer/GraphOptimizer/GraphOptimizer.h"
#include "glow/Partitioner/Partitioner.h"
#include "glow/Runtime/Executor/ThreadPoolExecutor.h"
#include "glow/Runtime/Provisioner/Provisioner.h"
#include "glow/Runtime/RuntimeTypes.h"
#include "glow/Support/Support.h"

#include "llvm/Support/CommandLine.h"

#include <glog/logging.h>

#include <future>
#include <queue>

using namespace glow;
using namespace runtime;

namespace {
llvm::cl::OptionCategory hostManagerCat("HostManager Options");

llvm::cl::opt<std::string> loadBackendSpecificOptionsOpt(
    "load-backend-specific-opts",
    llvm::cl::desc("Load backend-specific options for compilation."),
    llvm::cl::value_desc("options.yaml"), llvm::cl::Optional,
    llvm::cl::cat(hostManagerCat));
} // namespace

HostManager::HostManager(const HostConfig &hostConfig) : config_(hostConfig) {}

HostManager::HostManager(
    std::vector<std::unique_ptr<DeviceConfig>> deviceConfigs) {
  // TODO: move all initialization out of constructor.
  TEMP_EXIT_ON_ERR(init(std::move(deviceConfigs)));
}

HostManager::HostManager(
    std::vector<std::unique_ptr<DeviceConfig>> deviceConfigs,
    const HostConfig &hostConfig)
    : config_(hostConfig) {
  // TODO: move all initialization out of constructor.
  TEMP_EXIT_ON_ERR(init(std::move(deviceConfigs)));
}

llvm::Expected<DAG &> HostManager::getNetworkDAG(llvm::StringRef network) {
  auto it = networks_.find(network);
  if (it == networks_.end()) {
    return MAKE_ERR(GlowErr::ErrorCode::RUNTIME_ERROR, "Network not found.");
  }
  return it->second.dag;
}

llvm::Error
HostManager::init(std::vector<std::unique_ptr<DeviceConfig>> configs) {
  DeviceIDTy deviceCount = 0;

  for (auto &config : configs) {
    if (!config->hasName()) {
      config->name = "config" + std::to_string(deviceCount);
    }

    devices_[deviceCount] = std::unique_ptr<DeviceManager>(
        DeviceManager::createDeviceManager(*config));

    RETURN_IF_ERR(devices_[deviceCount]->init());

    deviceCount++;
  }
  provisioner_.reset(new Provisioner(devices_));
  executor_.reset(new ThreadPoolExecutor(devices_, config_.executorThreads));

  return llvm::Error::success();
}

HostManager::~HostManager() { llvm::toString(clearHost()); }

llvm::Error HostManager::addNetwork(std::unique_ptr<Module> module,
                                    CompilationContext &cctx,
                                    bool saturateHost) {
  std::lock_guard<std::mutex> networkLock(networkLock_);
  auto functions = module->getFunctions();
  for (auto &F : functions) {
    std::string name = F->getName();
    auto it = networks_.find(name);
    if (it != networks_.end()) {
      return MAKE_ERR(GlowErr::ErrorCode::RUNTIME_ERROR,
                      "Failed to add network: already have a function called " +
                          name);
    }
  }

  // Load backend-specific options if specified.
  if (!loadBackendSpecificOptionsOpt.empty()) {
    if (cctx.backendOpts.backendSpecificOpts.size() != 0) {
      VLOG_EVERY_N(1, 1000) << "Warning: backendSpecificOpts is set via the "
                               "HostManager, ignoring previously set options.";
    }
    cctx.backendOpts.backendSpecificOpts =
        deserializeStrStrMapFromYaml(loadBackendSpecificOptionsOpt);
  }

  std::vector<DeviceInfo> deviceInfo;
  for (auto &device : devices_) {
    DeviceInfo info = device.second->getDeviceInfo();
    info.availableMemory = device.second->getAvailableMemory();
    info.backendName = device.second->getBackendName();
    info.nonSupportedNodes = device.second->getParamByName("nonSupportedNodes");
    info.supportedNodes = device.second->getParamByName("supportedNodes");
    deviceInfo.push_back(info);
  }
  // Perform a round of target-independent graph optimizations. This helps the
  // partitioner to do its job more efficiently.
  for (Function *F : module->getFunctions()) {
    RETURN_IF_ERR(optimizeFunctionBeforeLowering(F, cctx));
  }
  Partitioner partitioner(module.get(), deviceInfo, saturateHost);
  DAGListTy nodeList;
  ASSIGN_VALUE_OR_RETURN_ERR(nodeList, partitioner.partition(cctx));

  if (cctx.precisionConfig.quantMode == QuantizationMode::Profile) {
    // Since for profiling the provisioner will be reset, we only allow one
    // network in one HM.
    RETURN_ERR_IF_NOT(networks_.size() == 0,
                      "For quantization profiling flow, there can't be other "
                      "registered networks before this one");
    // For profiling, we use CPU backend. Overwrite Provisioner and Executor to
    // force the network is compiled and run in profilingBackend.
    // backend.
    size_t devicesNum = devices_.size();
    for (size_t i = 0; i < devicesNum; i++) {
      auto name = devices_[i]->getDeviceConfig().name;
      auto config = llvm::make_unique<DeviceConfig>(profilingBackend, name);
      devices_[i] = std::unique_ptr<DeviceManager>(
          DeviceManager::createDeviceManager(*config));
      RETURN_IF_ERR(devices_[i]->init());
    }
    provisioner_.reset(new Provisioner(devices_));
    executor_.reset(new ThreadPoolExecutor(devices_, config_.executorThreads));
  }

  RETURN_IF_ERR(provisioner_->provision(nodeList, *module, cctx));

  // Clear constants contents from the module then put it in a
  // shared_ptr to be shared between all of the networks created from each
  // function in the module.
  module->strip();
  auto sharedModule = std::shared_ptr<Module>(std::move(module));

  for (auto &node : nodeList) {
    auto &networkData = networks_[(node.root)->name];
    networkData.dag = std::move(node);
    networkData.module = sharedModule;
  }

  return llvm::Error::success();
}

llvm::Error HostManager::removeNetwork(llvm::StringRef networkName) {
  std::lock_guard<std::mutex> networkLock(networkLock_);
  auto networkIterator = networks_.find(networkName);
  if (networkIterator == networks_.end()) {
    return llvm::Error::success();
  }

  // Issue an error as there are outstanding runs for the network
  if (networkIterator->second.refcount != 0) {
    return MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_BUSY,
                    llvm::formatv("Cannot remove the network {0}, as there are "
                                  "still outstanding runs",
                                  networkName)
                        .str());
  }

  OneErrOnly err;
  auto &nodes = networkIterator->second.dag.nodes;
  for (auto &node : nodes) {
    for (auto device : node->deviceIDs) {
      std::promise<void> removeNetwork;
      auto done = removeNetwork.get_future();
      std::unique_ptr<llvm::Error> removeErr;
      devices_[device]->evictNetwork(
          node->name,
          [&removeNetwork, &removeErr](std::string name, llvm::Error err) {
            removeErr = llvm::make_unique<llvm::Error>(std::move(err));
            removeNetwork.set_value();
          });
      done.get();
      err.set(std::move(*DCHECK_NOTNULL(removeErr.get())));
    }
    // Also remove compiledFunction from Provisioner.
    provisioner_->removeFunction(node->name);
  }
  networks_.erase(networkIterator);

  return err.get();
}

bool HostManager::networkAdded(llvm::StringRef networkName) {
  std::lock_guard<std::mutex> networkLock(networkLock_);
  return networks_.find(networkName) != networks_.end();
}

llvm::Error HostManager::clearHost() {
  // shutdown the executor, blocking on any current inflight and prevent new
  // requests from being serviced.
  executor_->shutdown();

  DCHECK_EQ(activeRequestCount_, 0)
      << "All requests should be finished when shutting down HostManager.";

  // Remove all networks from the host and device(s).
  while (networks_.size() != 0) {
    RETURN_IF_ERR(removeNetwork(networks_.begin()->first));
  }

  // Now it's safe to stop the DeviceManagers.
  std::lock_guard<std::mutex> networkLock(networkLock_);
  OneErrOnly errContainer;
  for (auto &it : devices_) {
    errContainer.set(it.second->stop());
  }

  return errContainer.get();
}

llvm::Error HostManager::runNetworkBlocking(llvm::StringRef networkName,
                                            PlaceholderBindings &bindings) {
  std::unique_ptr<PlaceholderBindings> phBindings(&bindings);
  std::unique_ptr<ExecutionContext> context =
      llvm::make_unique<ExecutionContext>(std::move(phBindings));
  std::promise<void> runPromise;
  auto fut = runPromise.get_future();
  std::unique_ptr<llvm::Error> runErr;
  runNetwork(
      networkName, std::move(context),
      [&runPromise, &runErr](runtime::RunIdentifierTy, llvm::Error err,
                             std::unique_ptr<ExecutionContext> contextPtr) {
        // Don't delete ph bindings since they were created from a passed in
        // reference.
        std::unique_ptr<PlaceholderBindings> phBind =
            contextPtr->movePlaceholderBindings();
        phBind.release();

        runErr = llvm::make_unique<llvm::Error>(std::move(err));
        runPromise.set_value();
      });

  fut.wait();
  return std::move(*DCHECK_NOTNULL(runErr.get()));
}

RunIdentifierTy
HostManager::runNetwork(llvm::StringRef networkName,
                        std::unique_ptr<ExecutionContext> context,
                        ResultCBTy callback) {
  DCHECK(callback != nullptr);

  TRACE_EVENT_SCOPE(context->getTraceContext(), TraceLevel::RUNTIME,
                    "HostManager::runNetwork");
  auto currentRun = totalRequestCount_++;

  NetworkData *network = nullptr;
  {
    std::lock_guard<std::mutex> networkLock(networkLock_);
    auto it = networks_.find(networkName);
    if (it != networks_.end()) {
      network = &it->second;
      network->refcount++;
    }
  }

  if (network == nullptr) {
    callback(
        currentRun,
        MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_NOT_FOUND,
                 llvm::formatv("Function {0} not found", networkName).str()),
        std::move(context));
    return currentRun;
  }

  size_t activeRequestCount = activeRequestCount_++;
  if (activeRequestCount >= config_.maxActiveRequests) {
    activeRequestCount_--;
    network->refcount--;
    callback(
        currentRun,
        MAKE_ERR(GlowErr::ErrorCode::RUNTIME_REQUEST_REFUSED,
                 strFormat("The number of allowed requests has been exceeded. "
                           "active requests: %lu allowed requests: %zu",
                           activeRequestCount, config_.maxActiveRequests)),
        std::move(context));
    return currentRun;
  }

  executor_->run(networks_[networkName].dag.root.get(), std::move(context),
                 currentRun,
                 [this, callback, name = networkName.str()](
                     RunIdentifierTy runID, llvm::Error err,
                     std::unique_ptr<ExecutionContext> context) {
                   {
                     std::lock_guard<std::mutex> networkLock(networkLock_);
                     auto it = networks_.find(name);
                     if (it != networks_.end()) {
                       it->second.refcount--;
                     }
                   }
                   TRACE_EVENT_INSTANT(context->getTraceContext(),
                                       TraceLevel::RUNTIME, "finish_" + name);
                   callback(runID, std::move(err), std::move(context));
                   --activeRequestCount_;
                 });
  return currentRun;
}
