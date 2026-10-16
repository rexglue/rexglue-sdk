#include "runtime/thread_registry.h"
#include "runtime/guest_stack.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include <format>

#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/process.h>

namespace rex::runtime {

ThreadRef::~ThreadRef() {
  registry->MarkHandlesClosed(thread_id);
}

ThreadRegistry::ThreadRegistry(Runtime& runtime, ObjectTable& table)
    : runtime_(runtime), table_(table) {}

ThreadRegistry::~ThreadRegistry() = default;

void ThreadRegistry::Dispose(std::unique_ptr<KernelThread> thread) {
  if (!thread) {
    return;
  }
  KernelThread* raw = thread.release();
  if (raw->pins.Retire()) {
    delete raw;
  }
}

Result<X_HANDLE> ThreadRegistry::Launch(std::unique_ptr<KernelThread> thread) {
  KernelThread* raw = thread.get();
  raw->thread_id_ = ++next_id_;
  raw->registry_ = this;
  ObjectBlock block = AllocObjectBlock(raw->GetMemory(), sizeof(X_KTHREAD));
  if (!block.address) {
    return Err<X_HANDLE>(StatusError(X_STATUS_NO_MEMORY));
  }
  raw->SetGuestObject(block.address);
  block.owned = false;
  Result<X_HANDLE> handle =
      table_.Emplace<ThreadRef>(AddOptions{{}, block}, raw->GetThreadId(), this);
  if (!handle) {
    return handle;
  }
  StashHandle(&raw->GetGuestObject<X_KTHREAD>()->header, *handle);
  raw->SetHandle(*handle);
  const X_STATUS status = raw->Create();
  if (XFAILED(status)) {
    raw->WriteExitState(status);
    CloseOwned(table_, *handle);
    return Err<X_HANDLE>(StatusError(status));
  }
  bool published = false;
  {
    std::unique_lock lock(mutex_);
    if (!raw->IsGuestThread() || !runtime_.IsTerminatingTitle()) {
      threads_.emplace(raw->GetThreadId(), std::move(thread));
      published = true;
    }
  }
  if (!published) {
    raw->Abandon();
    CloseOwned(table_, *handle);
    CloseOwned(table_, *handle);
    return Err<X_HANDLE>(StatusError(X_STATUS_PROCESS_IS_TERMINATING));
  }
  raw->Start();
  return handle;
}

ThreadPin ThreadRegistry::Find(u32 thread_id) {
  std::shared_lock lock(mutex_);
  auto it = threads_.find(thread_id);
  if (it == threads_.end()) {
    return {};
  }
  it->second->pins.Pin();
  return ThreadPin(it->second.get());
}

ThreadPin ThreadRegistry::FindByHandle(X_HANDLE handle) {
  auto ref = table_.Get<ThreadRef>(handle);
  if (!ref) {
    return {};
  }
  return Find(ref->thread_id);
}

std::vector<ThreadPin> ThreadRegistry::All() {
  std::vector<ThreadPin> results;
  std::shared_lock lock(mutex_);
  results.reserve(threads_.size());
  for (auto& [id, thread] : threads_) {
    thread->pins.Pin();
    results.emplace_back(thread.get());
  }
  return results;
}

void ThreadRegistry::MarkExited(KernelThread& thread) {
  std::unique_ptr<KernelThread> retired;
  {
    std::unique_lock lock(mutex_);
    thread.exited_ = true;
    if (thread.handles_closed_) {
      auto it = threads_.find(thread.GetThreadId());
      retired = std::move(it->second);
      threads_.erase(it);
    }
  }
  Dispose(std::move(retired));
}

void ThreadRegistry::MarkHandlesClosed(u32 thread_id) {
  std::unique_ptr<KernelThread> retired;
  {
    std::unique_lock lock(mutex_);
    auto it = threads_.find(thread_id);
    if (it == threads_.end()) {
      return;
    }
    it->second->handles_closed_ = true;
    if (it->second->exited_) {
      retired = std::move(it->second);
      threads_.erase(it);
    }
  }
  Dispose(std::move(retired));
}

std::optional<u32> ThreadRegistry::AllocateTLS(PPCContext* context) {
  if (!context) {
    REX_FAIL(system, std::nullopt, "AllocateTLS: null PPCContext");
  }

  auto globals =
      runtime_.GetMemory()->TranslateVirtual<KernelGuestGlobals*>(runtime_.GetKernelGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*runtime_.GetMemory(), context, tls_lock);

  std::optional<u32> result;

  auto current_thread = KernelThread::GetCurrentThread();
  if (!current_thread) {
    REXLOG_ERROR(system, "AllocateTLS: no current thread");
    kernel::xboxkrnl::KeKfReleaseSpinLock(*runtime_.GetMemory(), context, tls_lock, old_irql);
    return std::nullopt;
  }

  auto process_ptr = runtime_.GetMemory()->TranslateVirtual<X_KPROCESS*>(
      static_cast<u32>(current_thread->GetGuestObject<X_KTHREAD>()->process));
  if (!process_ptr) {
    REXLOG_ERROR(system, "AllocateTLS: failed to resolve current process");
    kernel::xboxkrnl::KeKfReleaseSpinLock(*runtime_.GetMemory(), context, tls_lock, old_irql);
    return std::nullopt;
  }

  for (u32 bitmap_index = 0; bitmap_index < 8; ++bitmap_index) {
    u32 bitmap_value = static_cast<u32>(process_ptr->tls_slot_bitmap[bitmap_index]);
    u32 leading_zeros = rex::lzcnt(bitmap_value);
    if (leading_zeros == 32) {
      continue;
    }

    u32 slot = bitmap_index * 32 + leading_zeros;
    if (slot >= 256) {
      continue;
    }

    u32 bit_index = 31 - leading_zeros;
    process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value & ~(1U << bit_index);
    result = slot;
    break;
  }

  kernel::xboxkrnl::KeKfReleaseSpinLock(*runtime_.GetMemory(), context, tls_lock, old_irql);
  return result;
}

void ThreadRegistry::FreeTLS(PPCContext* context, u32 slot) {
  if (!context) {
    REXLOG_ERROR(system, "FreeTLS: null PPCContext");
    return;
  }
  if (slot >= 256) {
    REXLOG_WARN(system, "FreeTLS: invalid TLS slot {}", slot);
    return;
  }

  auto current_thread = KernelThread::GetCurrentThread();
  if (!current_thread) {
    REXLOG_ERROR(system, "FreeTLS: no current thread");
    return;
  }

  auto current_kthread = current_thread->GetGuestObject<X_KTHREAD>();
  if (!current_kthread) {
    REXLOG_ERROR(system, "FreeTLS: failed to resolve current KTHREAD");
    return;
  }

  auto process_ptr = runtime_.GetMemory()->TranslateVirtual<X_KPROCESS*>(
      static_cast<u32>(current_kthread->process));
  if (!process_ptr) {
    REXLOG_ERROR(system, "FreeTLS: failed to resolve current process");
    return;
  }

  auto globals =
      runtime_.GetMemory()->TranslateVirtual<KernelGuestGlobals*>(runtime_.GetKernelGlobals());
  auto tls_lock = &globals->tls_lock;
  auto old_irql = kernel::xboxkrnl::KeKfAcquireSpinLock(*runtime_.GetMemory(), context, tls_lock);

  u32 bitmap_index = slot / 32;
  u32 bit_mask = 1U << (31 - (slot % 32));
  u32 bitmap_value = static_cast<u32>(process_ptr->tls_slot_bitmap[bitmap_index]);

  if (bitmap_value & bit_mask) {
    REXLOG_WARN(system, "FreeTLS: slot {} already free", slot);
    kernel::xboxkrnl::KeKfReleaseSpinLock(*runtime_.GetMemory(), context, tls_lock, old_irql);
    return;
  }

  const std::vector<ThreadPin> threads = All();
  u32 current_process_ptr = static_cast<u32>(current_kthread->process);

  for (const ThreadPin& thread : threads) {
    if (!thread || !thread->IsGuestThread()) {
      continue;
    }
    auto thread_kthread = thread->GetGuestObject<X_KTHREAD>();
    if (thread_kthread && static_cast<u32>(thread_kthread->process) == current_process_ptr) {
      thread->SetTLSValue(slot, 0);
    }
  }

  process_ptr->tls_slot_bitmap[bitmap_index] = bitmap_value | bit_mask;

  kernel::xboxkrnl::KeKfReleaseSpinLock(*runtime_.GetMemory(), context, tls_lock, old_irql);
}

std::optional<FiberInfo> ThreadRegistry::LookupFiber(u32 address) {
  std::lock_guard lock(fiber_mutex_);
  auto it = fiber_map_.find(address);
  return it != fiber_map_.end() ? std::optional<FiberInfo>(it->second) : std::nullopt;
}

void ThreadRegistry::RegisterFiber(u32 address, const FiberInfo& info) {
  std::lock_guard lock(fiber_mutex_);
  fiber_map_[address] = info;
}

void ThreadRegistry::UnregisterFiber(u32 address) {
  std::lock_guard lock(fiber_mutex_);
  fiber_map_.erase(address);
}

const char* ThreadRegistry::GetOrCreateFiberName(u32 address, const char* thread_name) {
  std::lock_guard lock(fiber_name_pool_mutex_);
  auto it = fiber_name_pool_.find(address);
  if (it != fiber_name_pool_.end()) {
    return it->second.get();
  }
  auto name = std::format("Fiber {:08X} ({})", address, thread_name);
  auto buf = std::make_unique<char[]>(name.size() + 1);
  std::memcpy(buf.get(), name.c_str(), name.size() + 1);
  const char* ptr = buf.get();
  fiber_name_pool_.emplace(address, std::move(buf));
  return ptr;
}

void ThreadRegistry::DestroyFibers() {
  decltype(fiber_map_) fibers;
  {
    std::lock_guard lock(fiber_mutex_);
    fibers.swap(fiber_map_);
  }
  for (auto& [guest_addr, info] : fibers) {
    if (info.host_fiber && !info.is_thread_fiber) {
      info.host_fiber->Destroy();
    }
    if (!info.is_thread_fiber) {
      FreeGuestStack(*runtime_.GetMemory(), info.guest_stack_bottom);
    }
    if (info.guest_context_addr) {
      runtime_.GetMemory()->SystemHeapFree(info.guest_context_addr);
    }
  }
}

void ThreadRegistry::RegisterThreadNotify(u32 notification) {
  std::lock_guard lock(notify_mutex_);
  if (std::ranges::find(thread_notifications_, notification) == thread_notifications_.end()) {
    thread_notifications_.push_back(notification);
  }
}

void ThreadRegistry::RemoveThreadNotify(u32 notification) {
  std::lock_guard lock(notify_mutex_);
  std::erase(thread_notifications_, notification);
}

std::vector<u32> ThreadRegistry::GetThreadNotifications() {
  std::lock_guard lock(notify_mutex_);
  return thread_notifications_;
}

}  // namespace rex::runtime
