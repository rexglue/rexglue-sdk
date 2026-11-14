#include "kernel/title_context.h"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>

#include <rex/kernel/title_runtime.h>
#include <rex/xam/achievement_manager.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/utility.h>
#include "kernel/title_state.h"

namespace rex::kernel {

namespace {

constexpr u64 kPhysicalSize = 0x20000000;
constexpr std::array<u32, 3> kPhysicalViewBases = {0xA0000000, 0xC0000000, 0xE0000000};
constexpr u32 kLastViewBase = 0xE0000000;
constexpr u32 kLastViewOffset = 0x1000;
constexpr u32 kHostThreadStackSize = 128 * 1024;

thread_local bool t_host_thread = false;

bool InPhysical(u64 address, u64 length) {
  return address + length <= kPhysicalSize;
}

Error OutOfRange() {
  return Error(ErrorCategory::kMemory, "the range is outside physical memory");
}

Error Misaligned() {
  return Error(ErrorCategory::kMemory, "the address is not 4 byte aligned");
}

Achievement ToAchievement(const Achievement& achievement,
                          const rex::xam::IAchievementManager& manager) {
  Achievement result = achievement;
  result.unlocked = manager.IsUnlocked(achievement.id);
  result.unlock_time = manager.GetUnlockTime(achievement.id).value_or(0);
  return result;
}

std::pair<u32, u32> WriteThunk(void* context, u32 start, u32 length, bool exact) {
  const PhysicalRange unwatch = static_cast<IPhysicalWriteListener*>(context)->OnPhysicalWrite(
      PhysicalRange{start, length}, exact);
  return {unwatch.address, unwatch.length};
}

}  // namespace

TitleContext::TitleContext(TitleRuntime& runtime) : runtime_(runtime) {}

TitleContext::~TitleContext() {
  Detach();
}

void TitleContext::Detach() {
  std::unique_lock lock(handle_mutex_);
  handle_released_.wait(lock, [this] { return live_handles_ == 0; });
}

void TitleContext::AddHandle() {
  std::lock_guard lock(handle_mutex_);
  ++live_handles_;
}

void TitleContext::ReleaseHandle() {
  {
    std::lock_guard lock(handle_mutex_);
    --live_handles_;
  }
  handle_released_.notify_all();
}

Result<void> TitleContext::CopyFromPhysical(u32 address, std::span<std::byte> out) {
  if (!InPhysical(address, out.size())) {
    return std::unexpected(OutOfRange());
  }
  std::memcpy(out.data(),
              StateOf(runtime_).GetAddressSpace()->TranslatePhysical<const u8*>(address),
              out.size());
  return {};
}

Result<void> TitleContext::CopyToPhysical(u32 address, std::span<const std::byte> data) {
  if (!InPhysical(address, data.size())) {
    return std::unexpected(OutOfRange());
  }
  kernel::GuestAddressSpace& memory = *StateOf(runtime_).GetAddressSpace();
  std::memcpy(memory.TranslatePhysical<u8*>(address), data.data(), data.size());
  if (!data.empty()) {
    const u32 length = static_cast<u32>(data.size());
    for (const u32 view_base : kPhysicalViewBases) {
      const u32 offset = view_base == kLastViewBase ? kLastViewOffset : 0;
      if (address < offset) {
        continue;
      }
      kernel::BaseHeap* heap = memory.LookupHeap(view_base + address - offset);
      if (heap != nullptr && heap->GetHeapType() == kernel::HeapType::kGuestPhysical) {
        static_cast<kernel::PhysicalHeap*>(heap)->TriggerCallbacks(view_base + address - offset,
                                                                   length, true, true);
      }
    }
  }
  return {};
}

Result<std::span<const std::byte>> TitleContext::GetReadOnlyPhysicalSpan(PhysicalRange range) {
  if (!InPhysical(range.address, range.length)) {
    return std::unexpected(OutOfRange());
  }
  return std::span<const std::byte>(
      StateOf(runtime_).GetAddressSpace()->TranslatePhysical<const std::byte*>(range.address),
      range.length);
}

Result<std::span<std::byte>> TitleContext::GetWritablePhysicalSpan(PhysicalRange range) {
  if (!InPhysical(range.address, range.length)) {
    return std::unexpected(OutOfRange());
  }
  return std::span<std::byte>(
      StateOf(runtime_).GetAddressSpace()->TranslatePhysical<std::byte*>(range.address),
      range.length);
}

Result<void> TitleContext::StoreRelease32(u32 address, u32 value) {
  if ((address & 3) != 0) {
    return std::unexpected(Misaligned());
  }
  if (!InPhysical(address, sizeof(u32))) {
    return std::unexpected(OutOfRange());
  }
  StateOf(runtime_).GetAddressSpace()->TranslatePhysical<be_u32*>(address)->Store(
      value, std::memory_order_release);
  return {};
}

Result<u32> TitleContext::LoadAcquire32(u32 address) {
  if ((address & 3) != 0) {
    return std::unexpected(Misaligned());
  }
  if (!InPhysical(address, sizeof(u32))) {
    return std::unexpected(OutOfRange());
  }
  return StateOf(runtime_).GetAddressSpace()->TranslatePhysical<const be_u32*>(address)->Load(
      std::memory_order_acquire);
}

ScopeGuard TitleContext::AddWriteListener(IPhysicalWriteListener& listener) {
  kernel::GuestAddressSpace* memory = StateOf(runtime_).GetAddressSpace();
  void* registration = memory->RegisterPhysicalMemoryInvalidationCallback(&WriteThunk, &listener);
  AddHandle();
  return ScopeGuard([this, memory, registration]() {
    memory->UnregisterPhysicalMemoryInvalidationCallback(registration);
    ReleaseHandle();
  });
}

void TitleContext::WatchPhysicalWrites(PhysicalRange range) {
  if (!InPhysical(range.address, range.length)) {
    return;
  }
  StateOf(runtime_).GetAddressSpace()->EnablePhysicalMemoryAccessCallbacks(
      range.address, range.length, true, false);
}

ScopeGuard TitleContext::LockWriteWatches() {
  return ScopeGuard(
      [lock = StateOf(runtime_).GetAddressSpace()->LockWatches()]() mutable { lock.unlock(); });
}

Result<void> TitleContext::RaiseGraphicsInterrupt(u32 source, u32 cpu) {
  if (!t_host_thread) {
    return Err(ErrorCategory::kRuntime, "the calling thread did not come from StartHostThread");
  }
  if (StateOf(runtime_).IsTerminatingTitle()) {
    return {};
  }
  StateOf(runtime_).GetFunctionDispatcher()->RaiseGraphicsInterrupt(source, cpu);
  return {};
}

Result<ScopeGuard> TitleContext::StartHostThread(std::string_view name,
                                                 MoveOnlyFunction<void(std::stop_token)> fn) {
  auto stop = std::make_shared<std::stop_source>();
  auto body = std::make_shared<MoveOnlyFunction<void(std::stop_token)>>(std::move(fn));
  auto thread = std::make_unique<kernel::KernelThread>(runtime_, kHostThreadStackSize, 0,
                                                       [body, token = stop->get_token()]() {
                                                         t_host_thread = true;
                                                         (*body)(token);
                                                         t_host_thread = false;
                                                         return 0;
                                                       });
  thread->SetName(name);
  auto launched = runtime_.GetThreadRegistry().Launch(std::move(thread));
  if (!launched) {
    return std::unexpected(launched.error());
  }
  const X_HANDLE handle = *launched;
  AddHandle();
  return ScopeGuard([this, stop, handle]() {
    stop->request_stop();
    runtime_.GetThreadRegistry().GetObjectWaiter().WaitGuestObject(handle, 0, 0, 0, nullptr);
    kernel::CloseOwned(runtime_.GetObjectTable(), handle);
    ReleaseHandle();
  });
}

std::vector<Achievement> TitleContext::GetAchievements() {
  rex::xam::IAchievementManager& manager = runtime_.GetAchievementManager();
  std::vector<Achievement> result;
  for (const Achievement& achievement : manager.ListAchievements()) {
    result.push_back(ToAchievement(achievement, manager));
  }
  return result;
}

Result<std::vector<std::byte>> TitleContext::GetAchievementIcon(u32 image_id) {
  for (const Achievement& achievement : runtime_.GetAchievementManager().ListAchievements()) {
    if (achievement.image_id != image_id || achievement.icon_path.empty()) {
      continue;
    }
    std::ifstream file(achievement.icon_path, std::ios::binary);
    if (!file) {
      break;
    }
    std::vector<std::byte> bytes;
    for (std::istreambuf_iterator<char> it(file), end; it != end; ++it) {
      bytes.push_back(static_cast<std::byte>(*it));
    }
    return bytes;
  }
  return Err<std::vector<std::byte>>(ErrorCategory::kNotFound, "no icon has the image id");
}

ScopeGuard TitleContext::OnAchievementUnlocked(MoveOnlyFunction<void(const Achievement&)> fn) {
  rex::xam::IAchievementManager& manager = runtime_.GetAchievementManager();
  auto shared = std::make_shared<MoveOnlyFunction<void(const Achievement&)>>(std::move(fn));
  ScopeGuard registration = manager.RegisterNotificationCallback(
      [&manager, shared](const rex::xam::AchievementEvent& event) {
        (*shared)(ToAchievement(event.achievement, manager));
      });
  AddHandle();
  return ScopeGuard([this, registration = std::move(registration)]() mutable {
    registration.Reset();
    ReleaseHandle();
  });
}

}  // namespace rex::kernel
