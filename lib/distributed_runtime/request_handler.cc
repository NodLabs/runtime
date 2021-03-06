/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- request_handler.cc - Request Handler --------------*- C++ -*--------===//
//
// This file contains implementation of RequestHandler class.
//
//===----------------------------------------------------------------------===//
#include "tfrt/distributed_runtime/request_handler.h"

#include <unordered_map>

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "tfrt/bef_converter/mlir_src_to_bef.h"
#include "tfrt/bef_executor/bef_file.h"
#include "tfrt/distributed_runtime/remote_object_manager.h"
#include "tfrt/host_context/async_dispatch.h"
#include "tfrt/host_context/concurrent_work_queue.h"
#include "tfrt/host_context/function.h"
#include "tfrt/host_context/host_allocator.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/tensor.h"
#include "tfrt/tensor/tensor_serialize_utils.h"

namespace tfrt {
// TODO(bramandia): Replace this with TFRT FunctionLibrary once available.
class RequestHandler::FunctionCache {
 public:
  explicit FunctionCache(HostContext* host_context) : host_(host_context) {}

  // Register the given program. A program can have multiple functions in it.
  // The program_name serves as both unique ID of this program.
  void Register(const std::string& program_name, BEFBuffer bef_buffer);

  // Create BEFFile corresponding to the program with the given name.
  RCReference<BEFFile> Prepare(const std::string& program_name);

 private:
  HostContext* host_;

  mutex cached_bef_mutex_;
  // A struct representing a BEFFile and the respective buffer.
  struct CachedBEF {
    RCReference<BEFFile> bef_file;
    BEFBuffer bef_buffer;
  };
  // Map from the program name to the CachedBEF.
  std::unordered_map<std::string, CachedBEF> cached_bef_
      TFRT_GUARDED_BY(cached_bef_mutex_);
};

void RequestHandler::FunctionCache::Register(const std::string& program_name,
                                             BEFBuffer bef_buffer) {
  RCReference<BEFFile> bef_file =
      tfrt::BEFFile::Open(bef_buffer, host_->GetKernelRegistry(),
                          host_->diag_handler(), host_->allocator());

  if (!bef_file) {
    TFRT_LOG(ERROR) << tfrt::StrCat("Failed to open lowered BEF for function ",
                                    program_name, ".");
    return;
  }
  mutex_lock lock(cached_bef_mutex_);
  cached_bef_[program_name].bef_file = bef_file.CopyRef();
  cached_bef_[program_name].bef_buffer = std::move(bef_buffer);
}

RCReference<BEFFile> RequestHandler::FunctionCache::Prepare(
    const std::string& program_name) {
  mutex_lock lock(cached_bef_mutex_);
  auto iter = cached_bef_.find(program_name);
  if (iter != cached_bef_.end()) {
    return iter->second.bef_file.CopyRef();
  }
  return RCReference<BEFFile>();
}

RequestHandler::RequestHandler(AsyncValueRef<DistributedContext> context)
    : dist_ctx_(context.GetAsyncValue()) {
  function_cache_ = std::make_unique<FunctionCache>(host_ctx());
}

RequestHandler::~RequestHandler() {}

void RequestHandler::HandleRemoteRegister(
    const RemoteRegisterInvocation& request) {
  auto bef_buffer = ConvertMLIRSrcToBEF(request.program,
                                        /* disable_optional_sections = */ true);
  if (bef_buffer.empty()) {
    // TODO(bramandia): Propagate errors to caller.
    TFRT_LOG(ERROR) << tfrt::StrCat("Failed to convert MLIR to BEF: ",
                                    request.program_name);
    return;
  }
  function_cache_->Register(request.program_name.str(), std::move(bef_buffer));
}

void RequestHandler::HandleRemoteExecute(const RemoteExecuteInvocation& request,
                                         RemoteExecuteCallbackFn done) {
  auto response = std::make_unique<RemoteExecuteInvocationResult>();
  response->ok = false;

  // TODO(bramandia): Propagate errors to caller.
  RCReference<BEFFile> bef_file =
      function_cache_->Prepare(request.program_name.str());
  if (bef_file.get() == nullptr) {
    TFRT_LOG(ERROR) << "Can't find program: [" << request.program_name << "]";
    done(std::move(response));
    return;
  }
  const Function* fn = bef_file->GetFunction(request.program_name);
  if (fn == nullptr) {
    TFRT_LOG(ERROR) << tfrt::StrCat(
        "Failed to get program from BEFFile with name ", request.program_name,
        ".");
    done(std::move(response));
    return;
  }
  if (fn->result_types().size() != request.outputs.size()) {
    TFRT_LOG(ERROR) << "Result size mismatch: fn #result: "
                    << fn->result_types().size()
                    << " Received #outputs: " << request.outputs.size();
    done(std::move(response));
    return;
  }

  // TODO(bramandia): Propagate RequestContext from the request.
  ResourceContext resource_context;
  RCReference<tfrt::RequestContext> req_ctx =
      RequestContext::Create(host_ctx(), &resource_context);

  tfrt::ExecutionContext exec_ctx{std::move(req_ctx)};

  RemoteObjectManager* manager = dist_ctx()->GetRemoteObjectManager();
  SmallVector<AsyncValue*, 4> arguments;
  SmallVector<RCReference<AsyncValue>, 4> arguments_ref;
  arguments.reserve(fn->argument_types().size());
  arguments_ref.reserve(fn->argument_types().size());
  int num_dist_context_args = 0;
  // Allow the first argument to be `DistributedContext`.
  if (fn->num_arguments() > 0 &&
      fn->argument_types()[0].GetName() == "!dist.dist_context") {
    arguments.push_back(dist_ctx_);
    num_dist_context_args = 1;
  }
  if (fn->argument_types().size() !=
      num_dist_context_args + request.inputs.size()) {
    TFRT_LOG(ERROR) << "Argument size mismatch: fn #arg: "
                    << fn->argument_types().size()
                    << " Received #inputs: " << request.inputs.size();
    return;
  }
  for (int i = 0; i < request.inputs.size(); ++i) {
    auto& id = request.inputs[i];

    RCReference<Device> device =
        host_ctx()->GetDeviceManager()->GetDeviceRef<Device>(id.device);
    if (device.get() == nullptr) {
      TFRT_LOG(ERROR) << "Can't find device: " << id.device;
      done(std::move(response));
      return;
    }
    RemoteObjectId input_id(id.prefix_id, id.local_id, device.CopyRef());
    RCReference<AsyncValue> val = manager->GetRemoteObject(input_id);
    arguments_ref.push_back(val.CopyRef());
    arguments.push_back(val.get());
  }
  auto results = std::make_unique<SmallVector<RCReference<AsyncValue>, 4>>();
  results->resize(fn->result_types().size());

  fn->Execute(exec_ctx, arguments, *results);
  for (int i = 0; i < request.outputs.size(); ++i) {
    auto& id = request.outputs[i].id;
    RCReference<Device> device =
        host_ctx()->GetDeviceManager()->GetDeviceRef<Device>(id.device);
    if (device.get() == nullptr) {
      TFRT_LOG(ERROR) << "Can't find device: " << id.device;
      done(std::move(response));
      return;
    }
    RemoteObjectId output_id(id.prefix_id, id.local_id, device.CopyRef());
    manager->SetRemoteObject(output_id, (*results)[i].CopyRef());
  }

  // get the pointer of results before being moved on the lambda capture.
  auto result_ref = results.get();
  // Request will live as long as done is not called yet.
  RunWhenReady(*result_ref, [fn, done = std::move(done), &request,
                             results = std::move(results),
                             response = std::move(response)]() mutable {
    for (int i = 0; i < request.outputs.size(); ++i) {
      if (request.outputs[i].need_metadata) {
        // TODO(bramandia): Handle function returning TensorHandle.
        if (fn->result_types()[i].GetName() == "!t.tensor") {
          std::string serialized =
              SerializeTensorMetadata((*results)[i]->get<Tensor>().metadata());
          response->metadata.push_back(serialized);
        } else {
          TFRT_LOG(ERROR) << "Invalid type " << fn->result_types()[i].GetName();
          done(std::move(response));
          return;
        }
      }
    }
    response->ok = true;
    done(std::move(response));
  });
}

HostContext* RequestHandler::host_ctx() {
  return dist_ctx_->get<DistributedContext>().GetHostContext();
}

DistributedContext* RequestHandler::dist_ctx() {
  return &dist_ctx_->get<DistributedContext>();
}
}  // namespace tfrt
