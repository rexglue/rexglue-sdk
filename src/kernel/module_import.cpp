#include <rex/logging/log.h>
#include <rex/kernel/module.h>

#include <mutex>

#include <rex/assert.h>
#include <rex/kernel/title_runtime.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/module_registry.h"

namespace rex::kernel {

PPCFunc* ResolveModuleImport(ModuleImport& target) {
  auto& registry = static_cast<ModuleRegistry&>(GetCurrent().GetModuleRegistry());
  const u64 generation = registry.GetGeneration();
  if (target.generation.load(std::memory_order_acquire) == generation) {
    if (PPCFunc* cached = target.function.load(std::memory_order_acquire)) {
      return cached;
    }
  }

  static std::mutex mutex;
  std::lock_guard lock(mutex);
  PPCFunc* function = registry.ResolveExport(target.library, target.ordinal);
  if (!function) {
    REX_FATAL(cpu, "Unresolved import of ordinal {} from {}", target.ordinal, target.library);
  }
  target.generation.store(0, std::memory_order_release);
  target.function.store(function, std::memory_order_release);
  target.generation.store(generation, std::memory_order_release);
  return function;
}

}  // namespace rex::kernel
