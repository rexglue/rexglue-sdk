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

#include "runtime/module_registry.h"

#include <filesystem>
#include <mutex>
#include <ranges>
#include <utility>

#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/guest_path.h>
#include <rex/runtime/kernel_module.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xthread.h>
#include <rex/string.h>

namespace rex::runtime {

namespace {

bool SameObject(const PinnedSlot& a, const PinnedSlot& b) {
  return &a.object() == &b.object();
}

}  // namespace

ModuleRegistry::ModuleRegistry(Runtime& runtime, ObjectTable& table)
    : runtime_(runtime), table_(table) {}

X_HANDLE ModuleRegistry::AddKernel(X_HANDLE handle) {
  Result<X_HANDLE> owned = table_.Duplicate(handle);
  if (!owned) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  kernel_modules_.push_back(*owned);
  return *owned;
}

bool ModuleRegistry::AddUser(X_HANDLE handle) {
  auto module = table_.Get<ModuleObject>(handle);
  if (!module) {
    return false;
  }
  for (X_HANDLE listed : user_modules()) {
    if (auto other = table_.Get<ModuleObject>(listed); other && other->path == module->path) {
      return false;
    }
  }
  Result<X_HANDLE> owned = table_.Duplicate(handle);
  if (!owned) {
    return false;
  }
  std::lock_guard lock(mutex_);
  user_modules_.push_back(*owned);
  return true;
}

bool ModuleRegistry::RemoveUser(X_HANDLE handle) {
  auto target = table_.Get(handle);
  if (!target) {
    return false;
  }
  X_HANDLE listed = 0;
  for (X_HANDLE candidate : user_modules()) {
    if (auto pin = table_.Get(candidate); pin && SameObject(pin, target)) {
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
  CloseOwned(table_, listed);
  return true;
}

std::optional<X_HANDLE> ModuleRegistry::FindKernel(std::string_view name) {
  for (X_HANDLE handle : kernel_modules()) {
    if (auto module = table_.Get<ModuleObject>(handle); module && ModuleMatches(*module, name)) {
      return handle;
    }
  }
  return std::nullopt;
}

std::optional<X_HANDLE> ModuleRegistry::FindUser(std::string_view name) {
  for (X_HANDLE handle : user_modules()) {
    if (auto module = table_.Get<ModuleObject>(handle); module && ModuleMatches(*module, name)) {
      return handle;
    }
  }
  return std::nullopt;
}

bool ModuleRegistry::IsKernel(std::string_view name) {
  return !name.empty() && FindKernel(name).has_value();
}

std::optional<X_HANDLE> ModuleRegistry::executable() const {
  std::lock_guard lock(mutex_);
  if (!executable_) {
    return std::nullopt;
  }
  return executable_;
}

void ModuleRegistry::set_executable(X_HANDLE handle) {
  X_HANDLE owned = 0;
  if (handle) {
    Result<X_HANDLE> duplicate = table_.Duplicate(handle);
    if (!duplicate) {
      return;
    }
    owned = *duplicate;
  }
  X_HANDLE previous = 0;
  {
    std::lock_guard lock(mutex_);
    previous = executable_;
    executable_ = owned;
  }
  if (previous) {
    CloseOwned(table_, previous);
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

void ModuleRegistry::EndLoad(const std::string& path) {
  std::lock_guard lock(mutex_);
  loading_paths_.erase(path);
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
    CloseOwned(table_, handle);
  }
  if (executable) {
    CloseOwned(table_, executable);
  }
  for (X_HANDLE handle : std::views::reverse(kernel)) {
    CloseOwned(table_, handle);
  }
}

void ModuleRegistry::UnloadAll() {
  const std::vector<X_HANDLE> loaded = user_modules();
  for (X_HANDLE module : std::views::reverse(loaded)) {
    UnloadUserModule(module, false);
  }
  Shutdown();
  std::lock_guard lock(recompiled_mutex_);
  if (!module_libraries_.empty()) {
    REXLOG_ERROR(system, "ModuleRegistry: {} recompiled libraries still loaded after module sweep",
                 module_libraries_.size());
    module_libraries_.clear();
  }
}

void ModuleRegistry::DrainLibraries() {
  std::lock_guard lock(recompiled_mutex_);
  deferred_unload_libraries_.clear();
}

std::optional<X_HANDLE> ModuleRegistry::GetModule(std::string_view name, bool user_only) {
  if (name.empty()) {
    return executable();
  } else if (rex::utf8_equal_case(name, "kernel32.dll")) {
    return std::nullopt;
  }

  if (!user_only) {
    if (auto kernel_module = FindKernel(name)) {
      return kernel_module;
    }
  }

  std::string path(name);
  if (auto entry = runtime_.file_system()->Stat(name)) {
    path = entry->absolute_path;
  }

  return FindUser(path);
}

Result<X_HANDLE> ModuleRegistry::LoadKernelModule(X_HANDLE created) {
  const X_HANDLE owned = AddKernel(created);
  if (!owned) {
    return Err<X_HANDLE>(StatusError(X_STATUS_INVALID_HANDLE));
  }
  return owned;
}

Result<X_HANDLE> ModuleRegistry::LoadUserModule(std::string_view raw_name, bool call_entry) {
  auto name = rex::utf8_find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    auto executable_module = table_.Get<ModuleObject>(executable().value_or(0));
    REX_ASSERT(executable_module);
    path =
        rex::utf8_join_guest_paths(rex::utf8_find_base_guest_path(executable_module->path), name);
  }

  if (auto existing = FindUser(path)) {
    return *existing;
  }
  if (!BeginLoad(path)) {
    REXLOG_WARN(system, "LoadUserModule: '{}' already being loaded by another thread", path);
    return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
  }

  struct LoadingMarkerGuard {
    ModuleRegistry* registry;
    const std::string& key;
    ~LoadingMarkerGuard() { registry->EndLoad(key); }
  } loading_guard{this, path};

  if (auto existing = FindUser(path)) {
    return *existing;
  }

  const X_HANDLE created = CreateUserModule(runtime_);
  if (!created) {
    return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  auto module = table_.Get<ModuleObject>(created);
  X_STATUS status = UserModuleLoadFromFile(runtime_, *module, path);
  if (XFAILED(status)) {
    CloseOwned(table_, created);
    return Err<X_HANDLE>(StatusError(status));
  }

  auto recomp = FindRecompiledModule(path);
  bool wired_recomp = false;
  if (recomp && !recomp->shared_lib_name.empty()) {
    const std::string& lib_key = recomp->guest_path;

    bool inserted;
    {
      std::lock_guard lock(recompiled_mutex_);
      inserted = module_libraries_.emplace(lib_key, rex::platform::DynamicLibrary{}).second;
    }
    if (!inserted) {
      REXLOG_ERROR(system, "Recompiled module '{}' already loaded; refusing duplicate load",
                   lib_key);
      CloseOwned(table_, created);
      return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
    }

    rex::platform::DynamicLibrary library_local;
    if (!library_local.Load(std::filesystem::path(recomp->shared_lib_name),
                            rex::platform::SymbolResolution::kImmediate)) {
      REXLOG_ERROR(system, "Failed to load shared library for module '{}'", recomp->pe_name);
    } else {
      auto register_func = reinterpret_cast<runtime::FunctionDispatcher::RegisterFn>(
          library_local.GetRawSymbol("ReXModule_Register"));
      using GetImageInfoFn = const rex::PPCImageInfo* (*)();
      auto get_image_info =
          reinterpret_cast<GetImageInfoFn>(library_local.GetRawSymbol("ReXModule_GetImageInfo"));
      if (!register_func) {
        REXLOG_ERROR(system, "ReXModule_Register not found in '{}'", recomp->shared_lib_name);
      } else if (!get_image_info) {
        REXLOG_ERROR(system, "ReXModule_GetImageInfo not found in '{}'", recomp->shared_lib_name);
      } else {
        auto* xex = UserModuleXEX(*module);
        const auto* image_info = get_image_info();
        if (!image_info) {
          REXLOG_ERROR(system, "ReXModule_GetImageInfo returned null for '{}'",
                       recomp->shared_lib_name);
        } else if (image_info->image_base != xex->base_address() ||
                   image_info->image_size != xex->image_size()) {
          REXLOG_ERROR(system,
                       "Recompiled module '{}' layout does not match loaded XEX "
                       "(DLL image={:08X}-{:08X}, XEX image={:08X}-{:08X})",
                       recomp->pe_name, image_info->image_base,
                       image_info->image_base + image_info->image_size, xex->base_address(),
                       xex->base_address() + xex->image_size());
        } else if (!runtime_.function_dispatcher()->InitializeFunctionTable(
                       image_info->code_base, image_info->code_size, image_info->image_base,
                       image_info->image_size)) {
          REXLOG_ERROR(system, "InitializeFunctionTable failed for module '{}'", recomp->pe_name);
        } else {
          runtime_.function_dispatcher()->RegisterModule(
              lib_key, image_info->code_base, register_func,
              image_info->recompiled_code ? image_info->recompiled_code() : std::span<const u8>());
          std::lock_guard lock(recompiled_mutex_);
          auto lib_it = module_libraries_.find(lib_key);
          REX_ASSERT(lib_it != module_libraries_.end());
          lib_it->second = std::move(library_local);
          wired_recomp = true;
        }
      }
    }

    if (!wired_recomp) {
      {
        std::lock_guard lock(recompiled_mutex_);
        module_libraries_.erase(lib_key);
      }
      CloseOwned(table_, created);
      return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
    }
  }

  if (!AddUser(created)) {
    CloseOwned(table_, created);
    if (auto existing = FindUser(path)) {
      return *existing;
    }
    return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  const X_HANDLE loaded = FindUser(module->path).value_or(0);

  if (module->user.is_dll_module && module->user.entry_point && call_entry) {
    if (!KernelThread::IsInThread()) {
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_ATTACH) skipped for '{}': not on a guest thread",
                  module->name);
    } else {
      auto* thread = KernelThread::GetCurrentThread();
      u64 args[] = {module->hmodule_ptr, 1 /* DLL_PROCESS_ATTACH */, 0};
      u64 dllmain_ret = runtime_.function_dispatcher()->Execute(thread->thread_state(),
                                                                module->user.entry_point, args, 3);
      if (static_cast<u32>(dllmain_ret) == 0) {
        REXLOG_ERROR(system,
                     "DllMain(DLL_PROCESS_ATTACH) returned FALSE for '{}'; rolling back load",
                     module->name);
        UnloadUserModule(loaded, /*call_entry=*/false);
        return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
      }
    }
  }

  return loaded;
}

void ModuleRegistry::UnloadUserModule(X_HANDLE module_handle, bool call_entry) {
  auto module = table_.Get<ModuleObject>(module_handle);
  if (!module) {
    REXLOG_ERROR(system, "UnloadUserModule: invalid module handle {:08X}", module_handle);
    return;
  }

  if (module->user.is_dll_module && module->user.entry_point && call_entry) {
    if (!KernelThread::IsInThread()) {
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_DETACH) skipped for '{}': not on a guest thread",
                  module->name);
    } else {
      auto* thread = KernelThread::GetCurrentThread();
      u64 args[] = {module->hmodule_ptr, 0 /* DLL_PROCESS_DETACH */, 0};
      runtime_.function_dispatcher()->Execute(thread->thread_state(), module->user.entry_point,
                                              args, 3);
    }
  }

  auto recomp = FindRecompiledModule(module->path);
  if (recomp) {
    const std::string& key = recomp->guest_path;
    auto cleared_range = runtime_.function_dispatcher()->UnregisterModule(key);
    if (cleared_range) {
      for (X_HANDLE kernel_handle : kernel_modules()) {
        if (auto kernel_module = table_.Get<ModuleObject>(kernel_handle)) {
          KernelModuleInvalidateThunkCacheInRange(*kernel_module, cleared_range->first,
                                                  cleared_range->second);
        }
      }
    }

    if (!recomp->shared_lib_name.empty()) {
      std::lock_guard lock(recompiled_mutex_);
      auto lib_it = module_libraries_.find(key);
      if (lib_it != module_libraries_.end()) {
        deferred_unload_libraries_.push_back(std::move(lib_it->second));
        module_libraries_.erase(lib_it);
      }
    }
  }

  const bool found_module = RemoveUser(module_handle);
  if (found_module) {
    CloseOwned(table_,
               ModuleHandleFromHModule(runtime_.memory()->TranslateVirtual(module->hmodule_ptr)));
  }

  if (!found_module) {
    REXLOG_ERROR(system, "UnloadUserModule: module '{}' not found in the module registry",
                 module->path);
  }
}

void ModuleRegistry::RegisterRecompiledModule(const char* pe_name, const char* guest_path,
                                              const char* shared_lib_name) {
  std::lock_guard lock(recompiled_mutex_);
  RecompiledModuleInfo info;
  info.pe_name = pe_name ? pe_name : "";
  info.guest_path = guest_path ? NormalizeGuestPath(guest_path) : "";
  info.shared_lib_name = shared_lib_name ? shared_lib_name : "";

  for (const auto& existing : recompiled_modules_) {
    if (existing.guest_path == info.guest_path) {
      REXLOG_ERROR(
          system,
          "RegisterRecompiledModule: duplicate guest_path '{}' (existing pe='{}', new pe='{}')",
          info.guest_path, existing.pe_name, info.pe_name);
      return;
    }
  }

  REXLOG_DEBUG(system, "Registered recompiled module: pe='{}' path='{}' lib='{}'", info.pe_name,
               info.guest_path, info.shared_lib_name);
  recompiled_modules_.push_back(std::move(info));
}

std::optional<ModuleRegistry::RecompiledModuleInfo> ModuleRegistry::FindRecompiledModule(
    std::string_view guest_path) {
  std::lock_guard lock(recompiled_mutex_);
  auto normalized = NormalizeGuestPath(guest_path);
  for (const auto& info : recompiled_modules_) {
    if (info.guest_path == normalized) {
      return info;
    }
  }
  return std::nullopt;
}

}  // namespace rex::runtime
