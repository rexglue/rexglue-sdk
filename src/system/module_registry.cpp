/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/module_registry.h>

#include <mutex>
#include <ranges>
#include <utility>

namespace rex::system {

namespace {

bool SameRecord(const PinnedSlot& a, const PinnedSlot& b) {
  return &a.record() == &b.record();
}

}  // namespace

X_HANDLE ModuleRegistry::AddKernel(X_HANDLE handle) {
  X_HANDLE owned = 0;
  if (XFAILED(table_->Duplicate(handle, &owned))) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  kernel_modules_.push_back(owned);
  return owned;
}

bool ModuleRegistry::AddUser(X_HANDLE handle) {
  auto module = table_->Get<ModuleObject>(handle);
  if (!module) {
    return false;
  }
  for (X_HANDLE listed : user_modules()) {
    if (auto other = table_->Get<ModuleObject>(listed); other && other->path == module->path) {
      return false;
    }
  }
  X_HANDLE owned = 0;
  if (XFAILED(table_->Duplicate(handle, &owned))) {
    return false;
  }
  std::lock_guard lock(mutex_);
  user_modules_.push_back(owned);
  return true;
}

bool ModuleRegistry::RemoveUser(X_HANDLE handle) {
  auto target = table_->Get(handle);
  if (!target) {
    return false;
  }
  X_HANDLE listed = 0;
  for (X_HANDLE candidate : user_modules()) {
    if (auto pin = table_->Get(candidate); pin && SameRecord(pin, target)) {
      listed = candidate;
      break;
    }
  }
  if (!listed) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (std::erase(user_modules_, listed) == 0) {
      return false;
    }
  }
  table_->Close(listed);
  return true;
}

X_HANDLE ModuleRegistry::FindKernel(std::string_view name) {
  for (X_HANDLE handle : kernel_modules()) {
    if (auto module = table_->Get<ModuleObject>(handle); module && ModuleMatches(*module, name)) {
      return handle;
    }
  }
  return 0;
}

X_HANDLE ModuleRegistry::FindUser(std::string_view name) {
  for (X_HANDLE handle : user_modules()) {
    if (auto module = table_->Get<ModuleObject>(handle); module && ModuleMatches(*module, name)) {
      return handle;
    }
  }
  return 0;
}

X_HANDLE ModuleRegistry::Find(std::string_view name, bool user_only) {
  if (!user_only) {
    if (X_HANDLE handle = FindKernel(name)) {
      return handle;
    }
  }
  return FindUser(name);
}

bool ModuleRegistry::IsKernel(std::string_view name) {
  return !name.empty() && FindKernel(name) != 0;
}

X_HANDLE ModuleRegistry::executable() const {
  std::lock_guard lock(mutex_);
  return executable_;
}

void ModuleRegistry::SetExecutable(X_HANDLE handle) {
  X_HANDLE owned = 0;
  if (handle && XFAILED(table_->Duplicate(handle, &owned))) {
    return;
  }
  X_HANDLE previous = 0;
  {
    std::lock_guard lock(mutex_);
    previous = executable_;
    executable_ = owned;
  }
  if (previous) {
    table_->Close(previous);
  }
}

std::vector<X_HANDLE> ModuleRegistry::kernel_modules() {
  std::lock_guard lock(mutex_);
  return kernel_modules_;
}

std::vector<X_HANDLE> ModuleRegistry::user_modules() {
  std::lock_guard lock(mutex_);
  return user_modules_;
}

bool ModuleRegistry::BeginLoad(std::string_view path) {
  std::lock_guard lock(mutex_);
  return loading_paths_.emplace(path).second;
}

void ModuleRegistry::EndLoad(std::string_view path) {
  std::lock_guard lock(mutex_);
  loading_paths_.erase(std::string(path));
}

void ModuleRegistry::Shutdown() {
  std::vector<X_HANDLE> kernel;
  std::vector<X_HANDLE> user;
  X_HANDLE executable = 0;
  {
    std::lock_guard lock(mutex_);
    kernel.swap(kernel_modules_);
    user.swap(user_modules_);
    executable = std::exchange(executable_, 0);
  }
  for (X_HANDLE handle : std::views::reverse(user)) {
    table_->Close(handle);
  }
  if (executable) {
    table_->Close(executable);
  }
  for (X_HANDLE handle : std::views::reverse(kernel)) {
    table_->Close(handle);
  }
}

}  // namespace rex::system
