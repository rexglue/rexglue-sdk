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

#include <rex/system/util/object_table.h>

#include <algorithm>
#include <mutex>
#include <shared_mutex>

#include <rex/logging.h>
#include <rex/system/xthread.h>

namespace rex::system {

ObjectRecord::~ObjectRecord() {
  if (owns_guest_block && memory != nullptr) {
    FreeGuestBlock(memory, guest_ptr);
  }
}

}  // namespace rex::system

namespace rex::system::util {

ObjectTable::ObjectTable(memory::Memory* memory) : memory_(memory) {}

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

X_STATUS ObjectTable::AllocateSlot(u32* out_slot) {
  const u32 capacity = static_cast<u32>(slots_.size());
  u32 slot = last_free_slot_;
  for (u32 scanned = 0; scanned < capacity; ++scanned) {
    if (slot != 0 && slots_[slot].record == nullptr) {
      *out_slot = slot;
      last_free_slot_ = slot;
      return X_STATUS_SUCCESS;
    }
    slot = (slot + 1) % capacity;
  }
  const u32 new_capacity = std::max(16 * 1024u, capacity * 2);
  slots_.resize(new_capacity);
  last_free_slot_ = std::max(capacity, 1u);
  *out_slot = last_free_slot_;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::Insert(std::unique_ptr<ObjectRecord> record, const AddOptions& options,
                             X_HANDLE* out_handle) {
  record->memory = memory_;
  record->guest_ptr = options.block.address;
  record->owns_guest_block = options.block.owned;
  X_HANDLE handle = 0;
  {
    std::unique_lock lock(mutex_);
    u32 index = 0;
    if (X_STATUS status = AllocateSlot(&index); XFAILED(status)) {
      return status;
    }
    handle = kHandleBase + (index << 2);
    ObjectRecord* raw = record.get();
    raw->handles.push_back(handle);
    slots_[index] = Slot{raw, 1};
    if (!options.name.empty() && XSUCCEEDED(AddName(options.name, handle))) {
      raw->name = std::string(options.name);
    }
    records_.emplace(raw, std::move(record));
  }
  if (out_handle) {
    *out_handle = handle;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::Duplicate(X_HANDLE handle, X_HANDLE* out_handle) {
  handle = Translate(handle);
  std::unique_lock lock(mutex_);
  Slot* source = FindSlot(handle);
  if (!source) {
    return X_STATUS_INVALID_HANDLE;
  }
  ObjectRecord* record = source->record;
  u32 index = 0;
  if (X_STATUS status = AllocateSlot(&index); XFAILED(status)) {
    return status;
  }
  const X_HANDLE copy = kHandleBase + (index << 2);
  slots_[index] = Slot{record, 1};
  record->handles.push_back(copy);
  if (out_handle) {
    *out_handle = copy;
  }
  return X_STATUS_SUCCESS;
}

ObjectRecord* ObjectTable::Detach(Slot& slot, X_HANDLE handle) {
  ObjectRecord* record = slot.record;
  slot = Slot{};
  auto handle_entry = std::find(record->handles.begin(), record->handles.end(), handle);
  if (handle_entry != record->handles.end()) {
    record->handles.erase(handle_entry);
  }
  if (!record->name.empty()) {
    RemoveName(record->name);
  }
  if (!record->handles.empty()) {
    return nullptr;
  }
  auto owned = records_.find(record);
  ObjectRecord* released = owned->second.release();
  records_.erase(owned);
  return released;
}

X_STATUS ObjectTable::Retain(X_HANDLE handle) {
  handle = Translate(handle);
  std::unique_lock lock(mutex_);
  Slot* slot = FindSlot(handle);
  if (!slot) {
    return X_STATUS_INVALID_HANDLE;
  }
  ++slot->ref_count;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::Close(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }
  ObjectRecord* retired = nullptr;
  {
    std::unique_lock lock(mutex_);
    Slot* slot = FindSlot(handle);
    if (!slot) {
      return X_STATUS_INVALID_HANDLE;
    }
    if (--slot->ref_count != 0) {
      return X_STATUS_SUCCESS;
    }
    retired = Detach(*slot, handle);
  }
  if (retired) {
    Retire(retired);
  }
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::Remove(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return X_STATUS_INVALID_HANDLE;
  }
  ObjectRecord* retired = nullptr;
  {
    std::unique_lock lock(mutex_);
    Slot* slot = FindSlot(handle);
    if (!slot) {
      return X_STATUS_INVALID_HANDLE;
    }
    slot->ref_count = 0;
    retired = Detach(*slot, handle);
  }
  if (retired) {
    Retire(retired);
  }
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::RetainPrimary(const PinnedSlot& slot) {
  std::unique_lock lock(mutex_);
  const ObjectRecord& record = slot.record();
  if (record.handles.empty()) {
    return X_STATUS_INVALID_HANDLE;
  }
  Slot* primary = FindSlot(record.handles.front());
  if (!primary) {
    return X_STATUS_INVALID_HANDLE;
  }
  ++primary->ref_count;
  return X_STATUS_SUCCESS;
}

X_STATUS ObjectTable::ReleasePrimary(const PinnedSlot& slot) {
  const X_HANDLE primary = PrimaryHandle(slot);
  return primary ? Close(primary) : X_STATUS_INVALID_HANDLE;
}

X_HANDLE ObjectTable::PrimaryHandle(const PinnedSlot& slot) {
  std::shared_lock lock(mutex_);
  const std::vector<X_HANDLE>& handles = slot.record().handles;
  return handles.empty() ? 0 : handles.front();
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

X_STATUS ObjectTable::AddName(std::string_view name, X_HANDLE handle) {
  std::lock_guard lock(name_mutex_);
  if (names_.count(string_key_case(name))) {
    return X_STATUS_OBJECT_NAME_COLLISION;
  }
  names_.insert({string_key_case::create(name), handle});
  return X_STATUS_SUCCESS;
}

void ObjectTable::RemoveName(std::string_view name) {
  std::lock_guard lock(name_mutex_);
  auto it = names_.find(string_key_case(name));
  if (it != names_.end()) {
    names_.erase(it);
  }
}

X_STATUS ObjectTable::OpenByName(std::string_view name, X_HANDLE* out_handle) {
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
    *out_handle = X_INVALID_HANDLE_VALUE;
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  *out_handle = handle;
  Slot* primary = FindSlot(slot->record->handles.front());
  ++primary->ref_count;
  return X_STATUS_SUCCESS;
}

}  // namespace rex::system::util
