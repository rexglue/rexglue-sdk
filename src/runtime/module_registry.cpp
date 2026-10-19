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

#include "core/module_loader.h"

#include <filesystem>
#include <mutex>
#include <ranges>
#include <utility>

#include <rex/image_info.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/platform/file.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/xthread.h>
#include <rex/string.h>

namespace rex::runtime {

namespace {

bool SameObject(const PinnedSlot& a, const PinnedSlot& b) {
  return &a.GetKernelObject() == &b.GetKernelObject();
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
  for (X_HANDLE listed : GetUserModules()) {
    if (auto other = table_.Get<ModuleObject>(listed);
        other && rex::utf8_equal_case(other->path, module->path)) {
      return false;
    }
  }
  Result<X_HANDLE> owned = table_.Duplicate(handle);
  if (!owned) {
    return false;
  }
  std::lock_guard lock(mutex_);
  user_modules_.push_back(*owned);
  generation_.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool ModuleRegistry::RemoveUser(X_HANDLE handle) {
  auto target = table_.Get(handle);
  if (!target) {
    return false;
  }
  X_HANDLE listed = 0;
  for (X_HANDLE candidate : GetUserModules()) {
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
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }
  CloseOwned(table_, listed);
  return true;
}

std::optional<X_HANDLE> ModuleRegistry::FindKernel(std::string_view name) {
  for (X_HANDLE handle : GetKernelModules()) {
    if (auto module = table_.Get<ModuleObject>(handle); module && module->Matches(name)) {
      return handle;
    }
  }
  return std::nullopt;
}

std::optional<X_HANDLE> ModuleRegistry::FindUser(std::string_view name) {
  for (X_HANDLE handle : GetUserModules()) {
    if (auto module = table_.Get<ModuleObject>(handle); module && module->Matches(name)) {
      return handle;
    }
  }
  return std::nullopt;
}

bool ModuleRegistry::IsKernel(std::string_view name) {
  return !name.empty() && FindKernel(name).has_value();
}

std::optional<X_HANDLE> ModuleRegistry::GetExecutable() const {
  std::lock_guard lock(mutex_);
  if (!executable_) {
    return std::nullopt;
  }
  return executable_;
}

void ModuleRegistry::SetExecutable(X_HANDLE handle) {
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

std::vector<X_HANDLE> ModuleRegistry::GetKernelModules() {
  std::lock_guard lock(mutex_);
  return kernel_modules_;
}

std::vector<X_HANDLE> ModuleRegistry::GetUserModules() {
  std::lock_guard lock(mutex_);
  return user_modules_;
}

bool ModuleRegistry::BeginLoad(std::string_view path) {
  std::lock_guard lock(mutex_);
  return loading_paths_.emplace(StringKeyCase::Create(path)).second;
}

void ModuleRegistry::EndLoad(const std::string& path) {
  std::lock_guard lock(mutex_);
  loading_paths_.erase(StringKeyCase::Create(path));
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
  const std::vector<X_HANDLE> loaded = GetUserModules();
  for (X_HANDLE module : std::views::reverse(loaded)) {
    if (table_.Get<ModuleObject>(module)) {
      UnloadUserModule(module, false);
    }
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
    return GetExecutable();
  } else if (rex::utf8_equal_case(name, "kernel32.dll")) {
    return std::nullopt;
  }

  if (!user_only) {
    if (auto kernel_module = FindKernel(name)) {
      return kernel_module;
    }
  }

  std::string path(name);
  if (auto entry = runtime_.GetFileSystem()->Stat(name)) {
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

Result<X_HANDLE> ModuleRegistry::FindImportLibrary(const ModuleObject& importer,
                                                   std::string_view importer_path,
                                                   const XEXModule::ImportLibrary& library,
                                                   bool call_entry) {
  for (X_HANDLE handle : GetUserModules()) {
    auto module = table_.Get<ModuleObject>(handle);
    if (module && rex::utf8_equal_case(rex::utf8_find_base_name_from_guest_path(module->path),
                                       library.name)) {
      REXLOG_DEBUG(system, "Import library '{}' of '{}' matched the loaded module '{}'",
                   library.file_name, importer.name, module->path);
      AcquireReference(*module);
      return handle;
    }
  }
  const std::string directory = rex::utf8_find_base_guest_path(importer_path);
  for (const std::string& candidate :
       {library.file_name, library.name + ".xex", library.name + ".dll"}) {
    const std::string path = rex::utf8_join_guest_paths(directory, candidate);
    if (!runtime_.GetFileSystem()->Stat(path)) {
      continue;
    }
    REXLOG_DEBUG(system, "Import library '{}' of '{}' matched the file '{}'", library.file_name,
                 importer.name, path);
    return LoadUserModule(path, call_entry);
  }
  std::vector<std::string> candidates;
  {
    std::lock_guard lock(mutex_);
    candidates = import_candidates_;
  }
  for (const std::string& candidate : candidates) {
    const std::string file_name = rex::utf8_find_name_from_guest_path(candidate);
    if (rex::utf8_equal_case(file_name, library.file_name) ||
        rex::utf8_equal_case(file_name, library.name + ".xex") ||
        rex::utf8_equal_case(file_name, library.name + ".dll")) {
      REXLOG_DEBUG(system, "Import library '{}' of '{}' matched the listed file '{}'",
                   library.file_name, importer.name, candidate);
      return LoadUserModule(candidate, call_entry);
    }
  }
  REXLOG_ERROR(system, "Import library '{}' of '{}' matches no loaded module or file",
               library.file_name, importer.name);
  return Err<X_HANDLE>(StatusError(X_STATUS_DLL_NOT_FOUND));
}

void ModuleRegistry::PatchImportSlot(u32 slot, u32 value) {
  auto* memory = runtime_.GetMemory();
  auto* heap = memory->LookupHeap(slot);
  const u32 page = slot & ~(heap->GetPageSize() - 1);
  u32 previous = 0;
  const bool unlocked =
      heap->Protect(page, heap->GetPageSize(), kMemoryProtectRead | kMemoryProtectWrite, &previous);
  *memory->TranslateVirtual<be_u32*>(slot) = value;
  if (unlocked) {
    heap->Protect(page, heap->GetPageSize(), previous);
  }
}

void ModuleRegistry::AcquireReference(ModuleObject& module) {
  std::lock_guard lock(load_count_mutex_);
  runtime_.GetMemory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module.hmodule_ptr)->load_count++;
}

void ModuleRegistry::ReleaseReference(X_HANDLE handle, bool call_entry) {
  auto module = table_.Get<ModuleObject>(handle);
  if (!module) {
    return;
  }
  bool last;
  {
    std::lock_guard lock(load_count_mutex_);
    auto* ldr_data =
        runtime_.GetMemory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(module->hmodule_ptr);
    last = (--ldr_data->load_count == 0);
  }
  if (last) {
    UnloadUserModule(handle, call_entry);
  }
}

void ModuleRegistry::ReleaseDependencies(ModuleObject& module, bool call_entry) {
  const std::vector<X_HANDLE> dependencies = std::move(module.user.dependencies);
  module.user.dependencies.clear();
  for (X_HANDLE dependency : std::views::reverse(dependencies)) {
    ReleaseReference(dependency, call_entry);
  }
}

X_STATUS ModuleRegistry::ResolveImports(ModuleObject& module, std::string_view path,
                                        bool call_entry) {
  const XEXModule* xex = module.GetXEX();
  if (!xex) {
    return X_STATUS_UNSUCCESSFUL;
  }
  for (const XEXModule::ImportLibrary& library : xex->GetImportLibraries()) {
    if (rex::runtime::ModuleFromName(library.name) || IsKernel(library.name)) {
      continue;
    }
    const Result<X_HANDLE> handle = FindImportLibrary(module, path, library, call_entry);
    if (!handle) {
      return ToXStatus(handle.error());
    }
    module.user.dependencies.push_back(*handle);
    auto dependency = table_.Get<ModuleObject>(*handle);
    if (!dependency) {
      return X_STATUS_DLL_NOT_FOUND;
    }
    for (const XEXModule::ImportLibraryFn& import : library.imports) {
      const XEXModule* exporter = dependency->GetXEX();
      const u32 address = exporter ? exporter->GetProcAddress(static_cast<u16>(import.ordinal)) : 0;
      if (!address) {
        REXLOG_ERROR(system, "Import of ordinal {} from '{}' by '{}' is not exported",
                     import.ordinal, library.file_name, module.name);
        return X_STATUS_DRIVER_ORDINAL_NOT_FOUND;
      }
      if (import.value_address) {
        PatchImportSlot(import.value_address, address);
      }
    }
  }
  return X_STATUS_SUCCESS;
}

PPCFunc* ModuleRegistry::ResolveExport(std::string_view library, u16 ordinal) {
  for (X_HANDLE handle : GetUserModules()) {
    auto module = table_.Get<ModuleObject>(handle);
    if (module &&
        rex::utf8_equal_case(rex::utf8_find_base_name_from_guest_path(module->path), library)) {
      const XEXModule* xex = module->GetXEX();
      const u32 address = xex ? xex->GetProcAddress(ordinal) : 0;
      return address ? runtime_.GetFunctionDispatcher()->GetFunction(address) : nullptr;
    }
  }
  return nullptr;
}

Result<X_HANDLE> ModuleRegistry::LoadUserModule(std::string_view raw_name, bool call_entry) {
  auto name = rex::utf8_find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    auto executable_module = table_.Get<ModuleObject>(GetExecutable().value_or(0));
    REX_ASSERT(executable_module);
    path =
        rex::utf8_join_guest_paths(rex::utf8_find_base_guest_path(executable_module->path), name);
  }

  if (auto existing = FindUser(path)) {
    AcquireReference(*table_.Get<ModuleObject>(*existing));
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
    AcquireReference(*table_.Get<ModuleObject>(*existing));
    return *existing;
  }

  const X_HANDLE created = ModuleObject::CreateUserModule(runtime_);
  if (!created) {
    return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  auto module = table_.Get<ModuleObject>(created);
  X_STATUS status = module->LoadFromFile(runtime_, path);
  if (XFAILED(status)) {
    CloseOwned(table_, created);
    return Err<X_HANDLE>(StatusError(status));
  }

  if (const X_STATUS imports = ResolveImports(*module, path, call_entry); XFAILED(imports)) {
    ReleaseDependencies(*module, call_entry);
    CloseOwned(table_, created);
    return Err<X_HANDLE>(StatusError(imports));
  }

  auto recomp = FindRecompiledModule(path);
  if (recomp) {
    module->user.recompiled_key = recomp->guest_path;
  }
  bool wired_recomp = false;
  if (recomp && !recomp->shared_lib_name.empty()) {
    const std::string& lib_key = recomp->guest_path;

    bool inserted;
    {
      std::lock_guard lock(recompiled_mutex_);
      inserted = module_libraries_.emplace(lib_key, rex::platform::DynamicLibrary{}).second;
    }
    if (!inserted) {
      REXLOG_ERROR(system, "Recompiled module '{}' already loaded, refusing duplicate load",
                   lib_key);
      ReleaseDependencies(*module, call_entry);
      CloseOwned(table_, created);
      return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
    }

    rex::platform::DynamicLibrary library_local;
    if (!library_local.Load(rex::platform::GetExecutableFolder() /
                                rex::detail::LibraryFileName(recomp->shared_lib_name),
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
        auto* xex = module->GetXEX();
        const auto* image_info = get_image_info();
        if (!xex) {
          REXLOG_ERROR(system, "Recompiled module '{}' is not an XEX", recomp->pe_name);
        } else if (!image_info) {
          REXLOG_ERROR(system, "ReXModule_GetImageInfo returned null for '{}'",
                       recomp->shared_lib_name);
        } else if (image_info->image_base != xex->GetBaseAddress() ||
                   image_info->image_size != xex->GetImageSize()) {
          REXLOG_ERROR(system,
                       "Recompiled module '{}' layout does not match loaded XEX "
                       "(DLL image={:08X}-{:08X}, XEX image={:08X}-{:08X})",
                       recomp->pe_name, image_info->image_base,
                       image_info->image_base + image_info->image_size, xex->GetBaseAddress(),
                       xex->GetBaseAddress() + xex->GetImageSize());
        } else if (!runtime_.GetFunctionDispatcher()->InitializeFunctionTable(
                       image_info->code_base, image_info->code_size, image_info->image_base,
                       image_info->image_size)) {
          REXLOG_ERROR(system, "InitializeFunctionTable failed for module '{}'", recomp->pe_name);
        } else {
          runtime_.GetFunctionDispatcher()->RegisterModule(
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
      ReleaseDependencies(*module, call_entry);
      CloseOwned(table_, created);
      return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
    }
  }

  if (!AddUser(created)) {
    ReleaseDependencies(*module, call_entry);
    CloseOwned(table_, created);
    if (auto existing = FindUser(path)) {
      AcquireReference(*table_.Get<ModuleObject>(*existing));
      return *existing;
    }
    return Err<X_HANDLE>(StatusError(X_STATUS_UNSUCCESSFUL));
  }
  const X_HANDLE loaded = FindUser(module->path).value_or(0);

  if (module->user.is_dll_module && module->user.entry_point && call_entry) {
    if (!KernelThread::IsInThread()) {
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_ATTACH) skipped for '{}', not on a kernel thread",
                  module->name);
    } else {
      auto* thread = KernelThread::GetCurrentThread();
      u64 args[] = {module->hmodule_ptr, 1 /* DLL_PROCESS_ATTACH */, 0};
      u64 dllmain_ret = runtime_.GetFunctionDispatcher()->Execute(
          thread->GetThreadState(), module->user.entry_point, args, 3);
      if (static_cast<u32>(dllmain_ret) == 0) {
        REXLOG_ERROR(system,
                     "DllMain(DLL_PROCESS_ATTACH) returned FALSE for '{}', rolling back load",
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
      REXLOG_WARN(system, "DllMain(DLL_PROCESS_DETACH) skipped for '{}', not on a kernel thread",
                  module->name);
    } else {
      auto* thread = KernelThread::GetCurrentThread();
      u64 args[] = {module->hmodule_ptr, 0 /* DLL_PROCESS_DETACH */, 0};
      runtime_.GetFunctionDispatcher()->Execute(thread->GetThreadState(), module->user.entry_point,
                                                args, 3);
    }
  }

  if (const std::string& key = module->user.recompiled_key; !key.empty()) {
    auto cleared_range = runtime_.GetFunctionDispatcher()->UnregisterModule(key);
    if (cleared_range) {
      for (const std::vector<X_HANDLE>& handles : {GetKernelModules(), GetUserModules()}) {
        for (X_HANDLE listed : handles) {
          if (auto cached = table_.Get<ModuleObject>(listed)) {
            cached->InvalidateThunkCache(cleared_range->first, cleared_range->second);
          }
        }
      }
    }

    std::lock_guard lock(recompiled_mutex_);
    auto lib_it = module_libraries_.find(key);
    if (lib_it != module_libraries_.end()) {
      deferred_unload_libraries_.push_back(std::move(lib_it->second));
      module_libraries_.erase(lib_it);
    }
  }

  const bool found_module = RemoveUser(module_handle);
  if (found_module) {
    CloseOwned(table_, ModuleObject::HandleFromHModule(
                           runtime_.GetMemory()->TranslateVirtual(module->hmodule_ptr)));
  }
  ReleaseDependencies(*module, call_entry);

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
  info.guest_path = guest_path ? rex::NormalizeGuestPath(guest_path) : "";
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

void ModuleRegistry::AddImportCandidate(std::string_view path) {
  std::lock_guard lock(mutex_);
  import_candidates_.emplace_back(path);
}

std::optional<ModuleRegistry::RecompiledModuleInfo> ModuleRegistry::FindRecompiledModule(
    std::string_view guest_path) {
  std::lock_guard lock(recompiled_mutex_);
  auto normalized = rex::NormalizeGuestPath(guest_path);
  for (const auto& info : recompiled_modules_) {
    if (info.guest_path == normalized) {
      return info;
    }
  }
  return std::nullopt;
}

}  // namespace rex::runtime
