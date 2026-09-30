#pragma once
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

#include <unordered_map>

#include <rex/runtime/exports.h>
#include <rex/system/xmodule.h>

namespace rex {
class Runtime;

namespace system {

class KernelState;

class KernelModule : public XModule {
 public:
  KernelModule(KernelState* kernel_state, const std::string_view path);
  ~KernelModule() override;

  const std::string& path() const override { return path_; }
  const std::string& name() const override { return name_; }

  uint32_t GetProcAddressByOrdinal(uint16_t ordinal, uint32_t caller_address = 0) override;
  uint32_t GetProcAddressByName(const std::string_view name) override;

  /**
   * Erase any cached thunks whose guest address falls in [lo, hi).
   */
  void InvalidateThunkCacheInRange(uint32_t lo, uint32_t hi);

  /**
   * Binds a variable export to the guest address that holds it.
   *
   * @param ordinal The variable ordinal.
   * @param address The guest address of the variable.
   */
  void SetVariable(uint16_t ordinal, uint32_t address) { variables_.Set(ordinal, address); }

  /**
   * Returns the address of a variable export.
   *
   * @param ordinal The variable ordinal.
   * @return The guest address, or 0 when the module never created the variable.
   */
  uint32_t Variable(uint16_t ordinal) const { return variables_.Get(ordinal); }

 protected:
  Runtime* emulator_;
  memory::Memory* memory_;

  std::string name_;
  std::string path_;

  rex::platform::global_critical_region global_critical_region_;

  // Cache of (caller_module_base, ordinal) -> thunk guest address.
  struct ThunkKey {
    uint32_t caller_module_base;
    uint16_t ordinal;
    bool operator==(const ThunkKey& other) const {
      return caller_module_base == other.caller_module_base && ordinal == other.ordinal;
    }
  };
  struct ThunkKeyHash {
    std::size_t operator()(const ThunkKey& k) const noexcept {
      return std::hash<uint64_t>{}((uint64_t(k.caller_module_base) << 16) | k.ordinal);
    }
  };
  std::unordered_map<ThunkKey, uint32_t, ThunkKeyHash> thunk_cache_;
  rex::runtime::ExportVariables variables_;
};

}  // namespace system
}  // namespace rex
