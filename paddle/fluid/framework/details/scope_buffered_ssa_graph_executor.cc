// Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/framework/details/scope_buffered_ssa_graph_executor.h"
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "paddle/fluid/framework/details/multi_devices_helper.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/variable_helper.h"
#include "paddle/fluid/platform/profiler.h"

namespace paddle {
namespace framework {
namespace details {

static void CollectUniqueAllocations(
    const Variable &var,
    std::unordered_set<memory::Allocation *> *allocation_set) {
  if (var.IsType<LoDTensor>()) {
    allocation_set->insert(var.Get<LoDTensor>().Holder().get());
  } else if (var.IsType<SelectedRows>()) {
    allocation_set->insert(var.Get<SelectedRows>().value().Holder().get());
  } else if (var.IsType<LoDTensorArray>()) {
    for (auto &t : var.Get<LoDTensorArray>()) {
      allocation_set->insert(t.Holder().get());
    }
  }
}

static void CollectUniqueAllocations(
    const Scope &scope,
    std::unordered_set<memory::Allocation *> *allocation_set) {
  for (auto &var_name : scope.LocalVarNames()) {
    CollectUniqueAllocations(*scope.FindVar(var_name), allocation_set);
  }

  for (auto *kid : scope.kids()) {
    CollectUniqueAllocations(*kid, allocation_set);
  }
}

static size_t GetScopeVarMemorySize(const Scope &scope) {
  std::unordered_set<memory::Allocation *> allocation_set;
  CollectUniqueAllocations(scope, &allocation_set);
  size_t memory_size = 0;
  for (auto *allocation : allocation_set) {
    if (allocation) {
      memory_size += allocation->size();
    }
  }
  return memory_size;
}

ScopeBufferedSSAGraphExecutor::ScopeBufferedSSAGraphExecutor(
    ExecutionStrategy strategy, std::vector<Scope *> local_scopes,
    std::vector<Scope *> local_exec_scopes, std::vector<VariableInfo> var_infos,
    std::vector<platform::Place> places,
    std::unique_ptr<SSAGraphExecutor> &&underlying_executor)
    : strategy_(std::move(strategy)),
      underlying_executor_(std::move(underlying_executor)),
      local_scopes_(std::move(local_scopes)),
      local_exec_scopes_(std::move(local_exec_scopes)),
      var_infos_(std::move(var_infos)),
      places_(std::move(places)) {
  PADDLE_ENFORCE_EQ(local_scopes_.size(), local_exec_scopes_.size());
  PrepareLocalExeScopes();
}

FeedFetchList ScopeBufferedSSAGraphExecutor::Run(
    const std::vector<std::string> &fetch_tensors) {
  if (drop_scope_counter_ == 0) {
    platform::RecordEvent e("InitLocalVars");
    InitVariables();
  }

  std::vector<framework::LoDTensor> fetch_data;
  std::exception_ptr eptr = nullptr;
  try {
    fetch_data = underlying_executor_->Run(fetch_tensors);
  } catch (...) {
    eptr = std::current_exception();
  }

  if (VLOG_IS_ON(5)) {
    for (auto *scope : local_exec_scopes_) {
      VLOG(5) << "Left "
              << string::HumanReadableSize(GetScopeVarMemorySize(*scope))
              << " on scope " << scope << " before deleting";
    }
  }

  ++drop_scope_counter_;
  if (drop_scope_counter_ == strategy_.num_iteration_per_drop_scope_) {
    DropLocalExeScopes();
  }

  if (VLOG_IS_ON(5)) {
    for (auto *scope : local_exec_scopes_) {
      VLOG(5) << "Left "
              << string::HumanReadableSize(GetScopeVarMemorySize(*scope))
              << " on scope " << scope << " after deleting";
    }
  }

  if (eptr) {
    std::rethrow_exception(eptr);
  } else {
    return fetch_data;
  }
}

void ScopeBufferedSSAGraphExecutor::InitVariables() {
  for (auto &info : tmp_var_infos_) {
    for (auto &pair : info) {
      InitializeVariable(pair.first, pair.second);
    }
  }

  const ir::Graph &graph = Graph();
  if (graph.Has(details::kProgramDescs)) {
    auto &program_descs =
        graph.Get<details::ProgramDescs>(details::kProgramDescs);
    // Init vars
    auto &fused_grad_vars = graph.Get<details::FusedVars>(details::kFusedVars);
    for (size_t i = 0; i < local_exec_scopes_.size(); ++i) {
      for (auto &var_name : fused_grad_vars) {
        auto var = local_exec_scopes_[i]->Var(var_name);
        var->GetMutable<LoDTensor>();
      }
    }

    for (auto &program_desc : program_descs) {
      for (auto &op_desc : program_desc.Block(0).AllOps()) {
        for (size_t i = 0; i < local_exec_scopes_.size(); ++i) {
          auto op = OpRegistry::CreateOp(*op_desc);
          op->Run(*local_exec_scopes_[i], places_[i]);
        }
      }
    }
  }
}

void ScopeBufferedSSAGraphExecutor::DropLocalExeScopes() {
  platform::RecordEvent drop_scope_event("DropLocalExeScopes");
  drop_scope_counter_ = 0;
  for (auto &p : places_) {
    platform::DeviceContextPool::Instance().Get(p)->Wait();
  }

  for (size_t i = 0; i < local_exec_scopes_.size(); ++i) {
    local_exec_scopes_[i]->EraseVarsExcept(preserve_vars_[i]);
    local_exec_scopes_[i]->DropKids();
    for (auto &preserve_var : preserve_vars_[i]) {
      preserve_var->Clear();
    }
    VLOG(3) << "Drop local execution scope: " << local_scopes_[i];
  }
}

void ScopeBufferedSSAGraphExecutor::PrepareLocalExeScopes() {
  // Create local scopes.
  preserve_vars_.resize(local_scopes_.size());
  tmp_var_infos_.resize(local_scopes_.size());

  for (auto it = local_scopes_.rbegin(); it != local_scopes_.rend(); ++it) {
    size_t idx = local_scopes_.size() - 1 - (it - local_scopes_.rbegin());
    auto *scope = local_scopes_[idx];
    auto *local_scope = local_exec_scopes_[idx];

    for (auto &info : var_infos_) {
      if (info.persistable_) {  // Persistable
        auto var = scope->FindVar(info.name_);
        if (var != nullptr) {
          VLOG(2)
              << info.name_
              << " has been initialized beforehand in global scope, skipped";
          continue;
        }
        InitializeVariable(scope->Var(info.name_), info.type_);
      } else {
        Variable *tmp_var = local_scope->Var(info.name_);
        preserve_vars_[idx].emplace(tmp_var);
        tmp_var_infos_[idx].emplace_back(tmp_var, info.type_);
      }
    }
  }
}

bool ScopeBufferedSSAGraphExecutor::NeedCreateLocalExeScope() {
  return drop_scope_counter_ == 0;
}

}  // namespace details
}  // namespace framework
}  // namespace paddle
