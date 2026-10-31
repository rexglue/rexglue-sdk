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

#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/platform/lock_order.h>
#include <rex/runtime/object_table.h>
#include <rex/runtime/xmemory.h>
#include <rex/string/key.h>

namespace rex::runtime {

/** The handle table of a runtime. */
class REX_RUNTIME_API ObjectTable final : public IObjectTable {
 public:
  /**
   * @param memory The guest memory that owned object blocks are freed from, null in tests.
   */
  explicit ObjectTable(Memory* memory);
  ~ObjectTable() override;

  /** Closes every handle and destroys every object. */
  void Reset();

  using IObjectTable::Get;
  using IObjectTable::GetAll;

  [[nodiscard]] Result<X_HANDLE> Add(std::unique_ptr<KernelObject> object,
                                     const AddOptions& options) override;
  [[nodiscard]] Result<X_HANDLE> AddWithBlock(std::unique_ptr<KernelObject> object,
                                              u32 attributes_ptr, u32 block_size) override;
  [[nodiscard]] Result<X_HANDLE> Duplicate(X_HANDLE handle) override;
  [[nodiscard]] Result<void> Retain(X_HANDLE handle) override;
  [[nodiscard]] Result<void> Close(X_HANDLE handle) override;
  [[nodiscard]] Result<void> Remove(X_HANDLE handle) override;
  [[nodiscard]] Result<void> RetainPrimary(const PinnedSlot& slot) override;
  [[nodiscard]] Result<void> ReleasePrimary(const PinnedSlot& slot) override;
  std::optional<X_HANDLE> GetPrimaryHandle(const PinnedSlot& slot) override;
  PinnedSlot Get(X_HANDLE handle) override;
  std::vector<PinnedSlot> GetAll() override;
  [[nodiscard]] Result<void> AddName(std::string_view name, X_HANDLE handle) override;
  [[nodiscard]] Result<X_HANDLE> OpenByName(std::string_view name) override;
  std::optional<X_HANDLE> ResolveHandle(u32 address, i32 as_type) override;
  std::optional<X_HANDLE> FindHandle(u32 address) override;

 private:
  struct Slot {
    KernelObject* object = nullptr;
    u32 ref_count = 0;
  };

  X_HANDLE Translate(X_HANDLE handle) const;
  Slot* FindSlot(X_HANDLE handle);
  u32 AllocSlot();
  KernelObject* Detach(Slot& slot, X_HANDLE handle);
  static void Retire(KernelObject* object);

  static constexpr u32 GetHandleSlot(X_HANDLE handle) { return (handle - kHandleBase) >> 2; }

  Memory* memory_;
  rex::platform::RankedMutex identity_mutex_{rex::platform::LockRank::kIdentity};
  rex::platform::RankedSharedMutex mutex_{rex::platform::LockRank::kTable};
  rex::platform::RankedMutex name_mutex_{rex::platform::LockRank::kName};
  std::vector<Slot> slots_;
  u32 last_free_slot_ = 0;
  std::unordered_map<KernelObject*, std::unique_ptr<KernelObject>> objects_;
  std::unordered_map<StringKeyIgnoreCase, X_HANDLE> names_;
  std::unordered_map<u32, KernelObject*> block_objects_;
};

}  // namespace rex::runtime
