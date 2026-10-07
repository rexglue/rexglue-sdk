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

#include "runtime/object_table.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xevent.h>
#include <rex/runtime/xmutant.h>
#include <rex/runtime/xobject.h>
#include <rex/runtime/xsemaphore.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtimer.h>

namespace rex::runtime {

ObjectRecord::~ObjectRecord() {
  if (owns_guest_block && memory != nullptr) {
    FreeGuestBlock(memory, guest_ptr, workspace);
  }
}

ObjectTable::ObjectTable(Memory* memory) : memory_(memory) {}

ObjectTable::~ObjectTable() {
  Reset();
}

void ObjectTable::Reset() {
  std::vector<ObjectRecord*> retired;
  {
    std::unique_lock lock(mutex_);
    std::lock_guard name_lock(name_mutex_);
    for (auto& [raw, owned] : records_) {
      retired.push_back(owned.release());
    }
    records_.clear();
    names_.clear();
    slots_.clear();
    last_free_slot_ = 0;
  }
  for (ObjectRecord* record : retired) {
    Retire(record);
  }
}

void ObjectTable::Retire(ObjectRecord* record) {
  if (record->pins.Retire()) {
    delete record;
  }
}

X_HANDLE ObjectTable::Translate(X_HANDLE handle) const {
  if (handle == 0xFFFFFFFF) {
    return 0;
  }
  if (handle == 0xFFFFFFFE) {
    return ThreadRecord::IsInThread() ? ThreadRecord::GetCurrentThreadHandle() : 0;
  }
  return handle;
}

ObjectTable::Slot* ObjectTable::FindSlot(X_HANDLE handle) {
  if (handle < kHandleBase) {
    return nullptr;
  }
  const u32 index = GetHandleSlot(handle);
  if (index == 0 || index >= slots_.size()) {
    return nullptr;
  }
  Slot* slot = &slots_[index];
  return slot->record != nullptr ? slot : nullptr;
}

u32 ObjectTable::AllocateSlot() {
  const u32 capacity = static_cast<u32>(slots_.size());
  u32 slot = last_free_slot_;
  for (u32 scanned = 0; scanned < capacity; ++scanned) {
    if (slot != 0 && slots_[slot].record == nullptr) {
      last_free_slot_ = slot;
      return slot;
    }
    slot = (slot + 1) % capacity;
  }
  const u32 new_capacity = std::max(16 * 1024u, capacity * 2);
  slots_.resize(new_capacity);
  last_free_slot_ = std::max(capacity, 1u);
  return last_free_slot_;
}

Result<X_HANDLE> ObjectTable::Add(std::unique_ptr<ObjectRecord> record, const AddOptions& options) {
  record->memory = memory_;
  record->guest_ptr = options.block.address;
  record->owns_guest_block = options.block.owned;
  record->workspace = options.block.workspace;
  std::unique_lock lock(mutex_);
  const u32 index = AllocateSlot();
  const X_HANDLE handle = kHandleBase + (index << 2);
  ObjectRecord* raw = record.get();
  raw->handles.push_back(handle);
  slots_[index] = Slot{raw, 1};
  if (!options.name.empty() && AddName(options.name, handle)) {
    raw->name = std::string(options.name);
  }
  records_.emplace(raw, std::move(record));
  return handle;
}

Result<X_HANDLE> ObjectTable::AddWithBlock(std::unique_ptr<ObjectRecord> record, u32 attributes_ptr,
                                           u32 block_size) {
  GuestBlock block;
  if (block_size) {
    block = AllocateGuestBlock(memory_, block_size);
    if (!block.address) {
      return Err<X_HANDLE>(StatusError(X_STATUS_NO_MEMORY));
    }
  }
  const std::string name = NameFromAttributes(memory_, attributes_ptr);
  Result<X_HANDLE> handle = Add(std::move(record), AddOptions{name, block});
  if (!handle) {
    if (block.address) {
      FreeGuestBlock(memory_, block.address);
    }
    return handle;
  }
  if (block.address) {
    StashHandle(memory_->TranslateVirtual<X_DISPATCH_HEADER*>(block.address), *handle);
  }
  return handle;
}

Result<X_HANDLE> ObjectTable::Duplicate(X_HANDLE handle) {
  handle = Translate(handle);
  std::unique_lock lock(mutex_);
  Slot* source = FindSlot(handle);
  if (!source) {
    return Err<X_HANDLE>(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ObjectRecord* record = source->record;
  const u32 index = AllocateSlot();
  const X_HANDLE copy = kHandleBase + (index << 2);
  slots_[index] = Slot{record, 1};
  record->handles.push_back(copy);
  return copy;
}

ObjectRecord* ObjectTable::Detach(Slot& slot, X_HANDLE handle) {
  ObjectRecord* record = slot.record;
  slot = Slot{};
  auto handle_entry = std::find(record->handles.begin(), record->handles.end(), handle);
  if (handle_entry != record->handles.end()) {
    record->handles.erase(handle_entry);
  }
  if (!record->name.empty()) {
    std::lock_guard name_lock(name_mutex_);
    auto it = names_.find(string_key_case(record->name));
    if (it != names_.end() && it->second == handle) {
      if (record->handles.empty()) {
        names_.erase(it);
      } else {
        it->second = record->handles.front();
      }
    }
  }
  if (!record->handles.empty()) {
    return nullptr;
  }
  auto owned = records_.find(record);
  ObjectRecord* released = owned->second.release();
  records_.erase(owned);
  return released;
}

Result<void> ObjectTable::Retain(X_HANDLE handle) {
  handle = Translate(handle);
  std::unique_lock lock(mutex_);
  Slot* slot = FindSlot(handle);
  if (!slot) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ++slot->ref_count;
  return Ok();
}

Result<void> ObjectTable::Close(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ObjectRecord* retired = nullptr;
  {
    std::unique_lock lock(mutex_);
    Slot* slot = FindSlot(handle);
    if (!slot) {
      return Err(StatusError(X_STATUS_INVALID_HANDLE));
    }
    if (--slot->ref_count != 0) {
      return Ok();
    }
    retired = Detach(*slot, handle);
  }
  if (retired) {
    Retire(retired);
  }
  return Ok();
}

Result<void> ObjectTable::Remove(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ObjectRecord* retired = nullptr;
  {
    std::unique_lock lock(mutex_);
    Slot* slot = FindSlot(handle);
    if (!slot) {
      return Err(StatusError(X_STATUS_INVALID_HANDLE));
    }
    slot->ref_count = 0;
    retired = Detach(*slot, handle);
  }
  if (retired) {
    Retire(retired);
  }
  return Ok();
}

Result<void> ObjectTable::RetainPrimary(const PinnedSlot& slot) {
  std::unique_lock lock(mutex_);
  const ObjectRecord& record = slot.record();
  if (record.handles.empty()) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  Slot* primary = FindSlot(record.handles.front());
  if (!primary) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ++primary->ref_count;
  return Ok();
}

Result<void> ObjectTable::ReleasePrimary(const PinnedSlot& slot) {
  const std::optional<X_HANDLE> primary = PrimaryHandle(slot);
  return primary ? Close(*primary) : Err(StatusError(X_STATUS_INVALID_HANDLE));
}

std::optional<X_HANDLE> ObjectTable::PrimaryHandle(const PinnedSlot& slot) {
  std::shared_lock lock(mutex_);
  const std::vector<X_HANDLE>& handles = slot.record().handles;
  if (handles.empty()) {
    return std::nullopt;
  }
  return handles.front();
}

PinnedSlot ObjectTable::Get(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return {};
  }
  std::shared_lock lock(mutex_);
  Slot* slot = FindSlot(handle);
  if (!slot) {
    return {};
  }
  slot->record->pins.Pin();
  return PinnedSlot(slot->record, handle);
}

std::vector<PinnedSlot> ObjectTable::GetAll() {
  std::vector<PinnedSlot> results;
  std::shared_lock lock(mutex_);
  results.reserve(records_.size());
  for (auto& [raw, owned] : records_) {
    raw->pins.Pin();
    results.emplace_back(raw, raw->handles.empty() ? 0 : raw->handles.front());
  }
  return results;
}

Result<void> ObjectTable::AddName(std::string_view name, X_HANDLE handle) {
  std::lock_guard lock(name_mutex_);
  if (names_.count(string_key_case(name))) {
    return Err(StatusError(X_STATUS_OBJECT_NAME_COLLISION));
  }
  names_.insert({string_key_case::create(name), handle});
  return Ok();
}

Result<X_HANDLE> ObjectTable::OpenByName(std::string_view name) {
  std::unique_lock lock(mutex_);
  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  {
    std::lock_guard name_lock(name_mutex_);
    auto it = names_.find(string_key_case(name));
    if (it != names_.end()) {
      handle = it->second;
    }
  }
  Slot* slot = handle == X_INVALID_HANDLE_VALUE ? nullptr : FindSlot(handle);
  if (!slot) {
    return Err<X_HANDLE>(StatusError(X_STATUS_OBJECT_NAME_NOT_FOUND));
  }
  Slot* primary = FindSlot(slot->record->handles.front());
  ++primary->ref_count;
  return handle;
}

std::optional<X_HANDLE> ObjectTable::ResolveGuestObject(u32 address, i32 as_type) {
  assert_not_null(memory_);
  if (address == 0) {
    return std::nullopt;
  }
  void* native_ptr = memory_->TranslateVirtual(address);
  auto* header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  std::lock_guard lock(identity_mutex_);
  if (as_type == -1) {
    as_type = header->type;
  }
  if (header->wait_list_flink == kXObjSignature) {
    return X_HANDLE(header->wait_list_blink);
  }
  Result<X_HANDLE> added = Err<X_HANDLE>(StatusError(X_STATUS_NO_MEMORY));
  switch (as_type) {
    case 0:
    case 1:
      added = Emplace<EventObject>(AddOptions{}, *header);
      break;
    case DISPATCHER_MUTANT: {
      const auto* mutant = reinterpret_cast<const X_KMUTANT*>(native_ptr);
      const bool owned_here = static_cast<i32>(static_cast<u32>(header->signal_state)) <= 0 &&
                              ThreadRecord::IsInThread() &&
                              ThreadRecord::GetCurrentThread()->guest_object() ==
                                  static_cast<u32>(mutant->owner_thread);
      added = Emplace<MutantObject>(AddOptions{}, owned_here, address);
      break;
    }
    case 5: {
      added = Emplace<SemaphoreObject>(AddOptions{}, *reinterpret_cast<X_KSEMAPHORE*>(native_ptr));
      [[maybe_unused]] auto semaphore = Get<SemaphoreObject>(added.value_or(0));
      assert_true(semaphore && semaphore->semaphore);
      break;
    }
    case DISPATCHER_MANUAL_RESET_TIMER:
    case DISPATCHER_AUTO_RESET_TIMER:
      added = Emplace<TimerObject>(AddOptions{}, static_cast<u32>(as_type - 8));
      break;
    default:
      REXLOG_WARN(system, "Guest object at {:08X} has the unsupported type {}", address, as_type);
      return std::nullopt;
  }
  const X_HANDLE handle = added.value_or(0);
  StashHandle(header, handle);
  return handle;
}

}  // namespace rex::runtime
