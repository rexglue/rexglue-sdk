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

#include "kernel/object_table.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <rex/assert.h>
#include <rex/kernel/types/thread.h>
#include <rex/logging/log.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/event_object.h>
#include <rex/kernel/object/mutant_object.h>
#include <rex/kernel/object/semaphore_object.h>
#include <rex/kernel/object/timer_object.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/object/xam_object.h>
#include <rex/kernel/object/object_block.h>
#include <rex/kernel/object/kernel_object.h>
#include <rex/kernel/object/object_pin.h>
#include <rex/kernel/object/object_table.h>

namespace rex::kernel {

namespace {

bool TypeMatchesHeader(ObjectType type, u32 header_type) {
  switch (type) {
    case ObjectType::kEvent:
      return header_type == kDispatcherManualResetEvent || header_type == kDispatcherAutoResetEvent;
    case ObjectType::kSemaphore:
      return header_type == kDispatcherSemaphore;
    case ObjectType::kMutant:
      return header_type == kDispatcherMutant;
    case ObjectType::kTimer:
      return header_type == kDispatcherManualResetTimer || header_type == kDispatcherAutoResetTimer;
    default:
      return true;
  }
}

}  // namespace

KernelObject::~KernelObject() {
  if (owns_guest_block && memory != nullptr) {
    FreeObjectBlock(memory, guest_ptr, workspace);
  }
}

ObjectTable::ObjectTable(Memory* memory) : memory_(memory) {}

ObjectTable::~ObjectTable() {
  log::NoThrow(log::system, [this] { Reset(); });
}

void ObjectTable::Reset() {
  std::vector<KernelObject*> retired;
  {
    std::unique_lock lock(mutex_);
    std::lock_guard name_lock(name_mutex_);
    for (auto& [raw, owned] : objects_) {
      retired.push_back(owned.release());
    }
    objects_.clear();
    names_.clear();
    block_objects_.clear();
    slots_.clear();
    last_free_slot_ = 0;
  }
  for (KernelObject* object : retired) {
    Retire(object);
  }
}

void ObjectTable::Retire(KernelObject* object) {
  if (object->pins.Retire()) {
    delete object;
  }
}

X_HANDLE ObjectTable::Translate(X_HANDLE handle) const {
  if (handle == 0xFFFFFFFF) {
    return 0;
  }
  if (handle == 0xFFFFFFFE) {
    return KernelThread::IsInThread() ? KernelThread::GetCurrentThreadHandle() : 0;
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
  return slot->object != nullptr ? slot : nullptr;
}

u32 ObjectTable::AllocSlot() {
  const u32 capacity = static_cast<u32>(slots_.size());
  u32 slot = last_free_slot_;
  for (u32 scanned = 0; scanned < capacity; ++scanned) {
    if (slot != 0 && slots_[slot].object == nullptr) {
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

Result<X_HANDLE> ObjectTable::Add(std::unique_ptr<KernelObject> object, const AddOptions& options) {
  object->destroy = [](KernelObject* retired) noexcept { delete retired; };
  object->memory = memory_;
  object->guest_ptr = options.block.address;
  object->owns_guest_block = options.block.owned;
  object->workspace = options.block.workspace;
  std::unique_lock lock(mutex_);
  const u32 index = AllocSlot();
  const X_HANDLE handle = kHandleBase + (index << 2);
  KernelObject* raw = object.get();
  raw->handles.push_back(handle);
  slots_[index] = Slot{raw, 1};
  if (!options.name.empty() && AddName(options.name, handle)) {
    raw->name = std::string(options.name);
  }
  if (options.block.address) {
    block_objects_.insert_or_assign(options.block.address, raw);
  }
  objects_.emplace(raw, std::move(object));
  return handle;
}

ObjectBlock ObjectTable::AllocateObjectBlock(u32 size, ObjectBlockHeap heap) {
  return AllocObjectBlock(memory_, size,
                          heap == ObjectBlockHeap::kWorkspace ? workspace_ : nullptr);
}

Result<X_HANDLE> ObjectTable::AddEvent(bool manual_reset, bool initial_state) {
  return CreateObject<EventObject>(0, sizeof(X_KEVENT), manual_reset, initial_state);
}

Result<X_HANDLE> ObjectTable::AddXAMObject(std::unique_ptr<IXAMObject> body,
                                           const AddOptions& options) {
  return Add(std::make_unique<KernelObject>(std::in_place_type<XAMObject>, std::move(body)),
             options);
}

Result<X_HANDLE> ObjectTable::AddFile(std::unique_ptr<filesystem::IFile> file, bool synchronous) {
  std::unique_ptr<KernelObject> object = MakeObject<FileObject>(std::move(file), synchronous);
  std::get<FileObject>(object->data).owner = owner_;
  return Add(std::move(object), AddOptions{});
}

Result<X_HANDLE> ObjectTable::AddWithBlock(std::unique_ptr<KernelObject> object, u32 attributes_ptr,
                                           u32 block_size) {
  ObjectBlock block;
  if (block_size) {
    block = AllocObjectBlock(memory_, block_size);
    if (!block.address) {
      return Err<X_HANDLE>(StatusError(X_STATUS_NO_MEMORY));
    }
  }
  const std::string name = NameFromAttributes(memory_, attributes_ptr);
  Result<X_HANDLE> handle = Add(std::move(object), AddOptions{name, block});
  if (!handle) {
    if (block.address) {
      FreeObjectBlock(memory_, block.address);
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
  KernelObject* object = source->object;
  const u32 index = AllocSlot();
  const X_HANDLE copy = kHandleBase + (index << 2);
  slots_[index] = Slot{object, 1};
  object->handles.push_back(copy);
  return copy;
}

KernelObject* ObjectTable::Detach(Slot& slot, X_HANDLE handle) {
  KernelObject* object = slot.object;
  slot = Slot{};
  auto handle_entry = std::find(object->handles.begin(), object->handles.end(), handle);
  if (handle_entry != object->handles.end()) {
    object->handles.erase(handle_entry);
  }
  if (!object->name.empty()) {
    std::lock_guard name_lock(name_mutex_);
    auto it = names_.find(string::StringKeyIgnoreCase(object->name));
    if (it != names_.end() && it->second == handle) {
      if (object->handles.empty()) {
        names_.erase(it);
      } else {
        it->second = object->handles.front();
      }
    }
  }
  if (!object->handles.empty()) {
    return nullptr;
  }
  if (auto block = block_objects_.find(object->guest_ptr);
      block != block_objects_.end() && block->second == object) {
    block_objects_.erase(block);
  }
  auto owned = objects_.find(object);
  KernelObject* released = owned->second.release();
  objects_.erase(owned);
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
  KernelObject* retired = nullptr;
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
  KernelObject* retired = nullptr;
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

Result<void> ObjectTable::RetainPrimary(const ObjectPin& slot) {
  std::unique_lock lock(mutex_);
  const KernelObject& object = slot.GetKernelObject();
  if (object.handles.empty()) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  Slot* primary = FindSlot(object.handles.front());
  if (!primary) {
    return Err(StatusError(X_STATUS_INVALID_HANDLE));
  }
  ++primary->ref_count;
  return Ok();
}

Result<void> ObjectTable::ReleasePrimary(const ObjectPin& slot) {
  const std::optional<X_HANDLE> primary = GetPrimaryHandle(slot);
  return primary ? Close(*primary) : Err(StatusError(X_STATUS_INVALID_HANDLE));
}

std::optional<X_HANDLE> ObjectTable::GetPrimaryHandle(const ObjectPin& slot) {
  std::shared_lock lock(mutex_);
  const std::vector<X_HANDLE>& handles = slot.GetKernelObject().handles;
  if (handles.empty()) {
    return std::nullopt;
  }
  return handles.front();
}

ObjectPin ObjectTable::Get(X_HANDLE handle) {
  handle = Translate(handle);
  if (!handle) {
    return {};
  }
  std::shared_lock lock(mutex_);
  Slot* slot = FindSlot(handle);
  if (!slot) {
    return {};
  }
  slot->object->pins.Pin();
  return ObjectPin(slot->object, handle);
}

std::vector<ObjectPin> ObjectTable::GetAll() {
  std::vector<ObjectPin> results;
  std::shared_lock lock(mutex_);
  results.reserve(objects_.size());
  for (auto& [raw, owned] : objects_) {
    raw->pins.Pin();
    results.emplace_back(raw, raw->handles.empty() ? 0 : raw->handles.front());
  }
  return results;
}

Result<void> ObjectTable::AddName(std::string_view name, X_HANDLE handle) {
  std::lock_guard lock(name_mutex_);
  if (names_.count(string::StringKeyIgnoreCase(name))) {
    return Err(StatusError(X_STATUS_OBJECT_NAME_COLLISION));
  }
  names_.insert({string::StringKeyIgnoreCase::Create(name), handle});
  return Ok();
}

Result<X_HANDLE> ObjectTable::OpenByName(std::string_view name) {
  std::unique_lock lock(mutex_);
  X_HANDLE handle = kXInvalidHandleValue;
  {
    std::lock_guard name_lock(name_mutex_);
    auto it = names_.find(string::StringKeyIgnoreCase(name));
    if (it != names_.end()) {
      handle = it->second;
    }
  }
  Slot* slot = handle == kXInvalidHandleValue ? nullptr : FindSlot(handle);
  if (!slot) {
    return Err<X_HANDLE>(StatusError(X_STATUS_OBJECT_NAME_NOT_FOUND));
  }
  Slot* primary = FindSlot(slot->object->handles.front());
  ++primary->ref_count;
  return handle;
}

std::optional<X_HANDLE> ObjectTable::FindHandle(u32 address) {
  std::shared_lock lock(mutex_);
  if (auto block = block_objects_.find(address); block != block_objects_.end()) {
    return block->second->handles.front();
  }
  return std::nullopt;
}

void ObjectTable::ForgetAddress(u32 address) {
  X_HANDLE handle = 0;
  {
    std::shared_lock table_lock(mutex_);
    if (auto block = block_objects_.find(address); block != block_objects_.end()) {
      handle = block->second->handles.front();
    }
  }
  if (!handle) {
    return;
  }
  if (auto timer = Get<TimerObject>(handle)) {
    timer->Cancel();
  }
  (void)Close(handle);
}

std::optional<X_HANDLE> ObjectTable::ResolveHandle(u32 address, i32 as_type) {
  REX_ASSERT(memory_ != nullptr);
  if (address == 0) {
    return std::nullopt;
  }
  std::lock_guard lock(identity_mutex_);
  void* native_ptr = memory_->TranslateVirtual(address);
  auto* header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  bool stale = false;
  {
    std::shared_lock table_lock(mutex_);
    if (auto block = block_objects_.find(address); block != block_objects_.end()) {
      if (block->second->owns_guest_block ||
          TypeMatchesHeader(block->second->GetType(), header->type)) {
        return block->second->handles.front();
      }
      stale = true;
    }
  }
  if (stale) {
    ForgetAddress(address);
  }
  if (as_type == -1) {
    as_type = header->type;
  }
  if (header->wait_list_flink == kStashSignature) {
    return static_cast<X_HANDLE>(header->wait_list_blink);
  }
  switch (as_type) {
    case 0:
    case 1:
      return Emplace<EventObject>(AddOptions{{}, ObjectBlock{address}}, *header).value_or(0);
    case kDispatcherMutant: {
      const auto* mutant = reinterpret_cast<const X_KMUTANT*>(native_ptr);
      const bool owned_here = static_cast<i32>(static_cast<u32>(header->signal_state)) <= 0 &&
                              KernelThread::IsInThread() &&
                              KernelThread::GetCallingThread()->GetGuestObject() ==
                                  static_cast<u32>(mutant->owner_thread);
      return Emplace<MutantObject>(AddOptions{{}, ObjectBlock{address}}, owned_here, address)
          .value_or(0);
    }
    case 5: {
      const X_HANDLE added = Emplace<SemaphoreObject>(AddOptions{{}, ObjectBlock{address}},
                                                      *reinterpret_cast<X_KSEMAPHORE*>(native_ptr))
                                 .value_or(0);
      [[maybe_unused]] auto semaphore = Get<SemaphoreObject>(added);
      REX_ASSERT(semaphore && semaphore->semaphore);
      return added;
    }
    case kDispatcherManualResetTimer:
    case kDispatcherAutoResetTimer:
      return Emplace<TimerObject>(AddOptions{{}, ObjectBlock{address}},
                                  static_cast<u32>(as_type - 8))
          .value_or(0);
    default:
      REXSYS_WARN("Guest object at {:08X} has the unsupported type {}", address, as_type);
      return std::nullopt;
  }
}

}  // namespace rex::kernel
