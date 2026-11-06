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
#include <rex/platform/lock_order.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/kernel_object.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/heap.h>
#include <rex/string/text.h>

namespace rex::kernel {

class ObjectTable final : public IObjectTable {
 public:
  /**
   * @param memory The guest memory that owned object blocks are freed from, null in tests.
   */
  explicit ObjectTable(GuestAddressSpace* memory);

  /**
   * Sets the runtime that the files created by the table use for reads and writes.
   *
   * @param owner The runtime, which outlives the table.
   */
  void SetOwner(TitleRuntime& owner) { owner_ = &owner; }
  ~ObjectTable() override;

  /** Closes every handle and destroys every object. */
  void Reset();

  /**
   * Sets the workspace that kWorkspace blocks come from.
   *
   * @param workspace The workspace, which outlives the table.
   */
  void SetWorkspace(WorkspaceHeap* workspace) { workspace_ = workspace; }

  using IObjectTable::Get;
  using IObjectTable::GetAll;

  [[nodiscard]] Result<X_HANDLE> Add(std::unique_ptr<KernelObject> object,
                                     const AddOptions& options) override;
  [[nodiscard]] Result<X_HANDLE> AddWithBlock(std::unique_ptr<KernelObject> object,
                                              u32 attributes_ptr, u32 block_size) override;
  [[nodiscard]] ObjectBlock AllocateObjectBlock(u32 size, ObjectType type,
                                                ObjectBlockHeap heap) override;
  [[nodiscard]] Result<X_HANDLE> AddEvent(bool manual_reset, bool initial_state) override;
  [[nodiscard]] Result<X_HANDLE> AddXAMObject(std::unique_ptr<IXAMObject> body,
                                              const AddOptions& options) override;
  [[nodiscard]] Result<X_HANDLE> AddFile(std::unique_ptr<filesystem::IFile> file,
                                         bool synchronous) override;
  [[nodiscard]] Result<X_HANDLE> Duplicate(X_HANDLE handle) override;
  [[nodiscard]] Result<X_HANDLE> Open(const ObjectPin& object) override;
  [[nodiscard]] Result<void> Close(X_HANDLE handle) override;
  [[nodiscard]] Result<void> Reference(const ObjectPin& object) override;
  [[nodiscard]] Result<void> Dereference(const ObjectPin& object) override;
  ObjectPin GetByAddress(u32 address) override;
  ObjectPin Get(X_HANDLE handle) override;
  std::vector<ObjectPin> GetAll() override;
  [[nodiscard]] Result<void> AddName(std::string_view name, X_HANDLE handle) override;
  [[nodiscard]] Result<X_HANDLE> OpenByName(std::string_view name) override;
  ObjectPin ResolveObject(u32 address, i32 as_type) override;
  void ForgetAddress(u32 address) override;

 private:
  struct Slot {
    KernelObject* object = nullptr;
  };

  u32 GetTypeAddress(ObjectType type) const;
  X_HANDLE Translate(X_HANDLE handle) const;
  Slot* FindSlot(X_HANDLE handle);
  u32 AllocSlot();
  X_HANDLE AddSlot(KernelObject* object);
  KernelObject* Register(std::unique_ptr<KernelObject> object, const AddOptions& options);
  KernelObject* Detach(KernelObject* object, X_HANDLE handle);
  KernelObject* Settle(KernelObject* object);
  static void Retire(KernelObject* object);

  static constexpr u32 GetHandleSlot(X_HANDLE handle) { return (handle - kHandleBase) >> 2; }

  GuestAddressSpace* memory_;
  WorkspaceHeap* workspace_ = nullptr;
  TitleRuntime* owner_ = nullptr;
  rex::platform::RankedMutex identity_mutex_{rex::platform::lock_rank::kObjectIdentityMap};
  rex::platform::RankedSharedMutex mutex_{rex::platform::lock_rank::kObjectTable};
  rex::platform::RankedMutex name_mutex_{rex::platform::lock_rank::kObjectNameMap};
  std::vector<Slot> slots_;
  u32 last_free_slot_ = 0;
  std::unordered_map<KernelObject*, std::unique_ptr<KernelObject>> objects_;
  std::unordered_map<string::StringKeyIgnoreCase, X_HANDLE> names_;
  std::unordered_map<u32, KernelObject*> block_objects_;
};

}  // namespace rex::kernel
