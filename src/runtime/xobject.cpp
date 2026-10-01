/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/runtime/xobject.h>

#include <chrono>
#include <mutex>
#include <type_traits>
#include <variant>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/runtime.h>
#include <rex/runtime/object_data.h>
#include <rex/runtime/util/string_utils.h>

namespace rex::runtime {

namespace {

struct WaitTarget {
  PinnedSlot slot;
  ThreadPin thread;
  rex::platform::WaitHandle* handle = nullptr;
  MutantObject* mutant = nullptr;
};

rex::platform::WaitHandle* HandleOf(ObjectData& data) {
  return std::visit(
      [](auto& object) -> rex::platform::WaitHandle* {
        using T = std::decay_t<decltype(object)>;
        if constexpr (std::is_same_v<T, EventObject>) {
          return object.event.get();
        } else if constexpr (std::is_same_v<T, SemaphoreObject>) {
          return object.semaphore.get();
        } else if constexpr (std::is_same_v<T, MutantObject>) {
          return object.mutant.get();
        } else if constexpr (std::is_same_v<T, TimerObject>) {
          return object.timer.get();
        } else if constexpr (std::is_same_v<T, NotifyListenerObject>) {
          return object.wait_handle.get();
        } else if constexpr (std::is_same_v<T, FileObject>) {
          return object.async_event.get();
        } else {
          return nullptr;
        }
      },
      data);
}

WaitTarget Resolve(Runtime& runtime, X_HANDLE handle) {
  WaitTarget target;
  target.slot = runtime.object_table().Get(handle);
  if (!target.slot) {
    return target;
  }
  if (auto* ref = std::get_if<ThreadRef>(&target.slot.data())) {
    target.thread = runtime.thread_registry().Find(ref->thread_id);
    target.handle = target.thread ? target.thread->thread() : nullptr;
  } else {
    target.handle = HandleOf(target.slot.data());
    target.mutant = std::get_if<MutantObject>(&target.slot.data());
  }
  return target;
}

std::chrono::milliseconds TimeoutOf(u64* opt_timeout) {
  return opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                           TimeoutTicksToMs(static_cast<i64>(*opt_timeout))))
                     : std::chrono::milliseconds::max();
}

X_STATUS MapResult(rex::platform::WaitResult result) {
  switch (result) {
    case rex::platform::WaitResult::kSuccess:
      return X_STATUS_SUCCESS;
    case rex::platform::WaitResult::kUserCallback:
      return X_STATUS_USER_APC;
    case rex::platform::WaitResult::kTimeout:
      rex::platform::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

}  // namespace

u32 TimeoutTicksToMs(i64 timeout_ticks) {
  if (timeout_ticks > 0) {
    assert_always();
    return 0;
  }
  if (timeout_ticks < 0) {
    return static_cast<u32>(-timeout_ticks / 10000);
  }
  return 0;
}

std::string NameFromAttributes(runtime::Memory* memory, u32 obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return {};
  }
  auto name = util::TranslateAnsiStringAddress(
      memory, memory::load_and_swap<u32>(memory->TranslateVirtual(obj_attributes_ptr + 4)));
  return std::string(name);
}

GuestBlock AllocateGuestBlock(runtime::Memory* memory, u32 size) {
  const u32 total_size = size + sizeof(X_OBJECT_HEADER);
  const u32 mem = memory->SystemHeapAlloc(total_size);
  if (!mem) {
    return {};
  }
  memory->Zero(mem, total_size);
  auto* header = memory->TranslateVirtual<X_OBJECT_HEADER*>(mem);
  const u32 object_type = memory->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    header->object_type_ptr = object_type;
  }
  return GuestBlock{mem + static_cast<u32>(sizeof(X_OBJECT_HEADER)), true};
}

void FreeGuestBlock(runtime::Memory* memory, u32 address) {
  if (!address) {
    return;
  }
  const u32 block = address - sizeof(X_OBJECT_HEADER);
  auto* header = memory->TranslateVirtual<X_OBJECT_HEADER*>(block);
  if (header->object_type_ptr) {
    memory->SystemHeapFree(header->object_type_ptr);
  }
  memory->SystemHeapFree(block);
}

X_STATUS WaitOn(Runtime& runtime, X_HANDLE handle, u32 wait_reason, u32 processor_mode,
                u32 alertable, u64* opt_timeout) {
  WaitTarget target = Resolve(runtime, handle);
  if (!target.slot) {
    return X_STATUS_INVALID_HANDLE;
  }
  if (!target.handle) {
    return X_STATUS_SUCCESS;
  }
  const auto timeout = TimeoutOf(opt_timeout);
  ThreadRecord::CheckTitleTermination();
  auto result = rex::platform::Wait(target.handle, alertable ? true : false, timeout);
  ThreadRecord::CheckTitleTermination();
  const X_STATUS status = MapResult(result);
  if (status == X_STATUS_SUCCESS && target.mutant) {
    MutantAcquired(*target.mutant);
  }
  return status;
}

X_STATUS SignalAndWaitOn(Runtime& runtime, X_HANDLE signal_handle, X_HANDLE wait_handle,
                         u32 wait_reason, u32 processor_mode, u32 alertable, u64* opt_timeout) {
  WaitTarget signal = Resolve(runtime, signal_handle);
  WaitTarget wait = Resolve(runtime, wait_handle);
  if (!signal.slot || !wait.slot) {
    return X_STATUS_INVALID_HANDLE;
  }
  auto result = rex::platform::SignalAndWait(signal.handle, wait.handle, alertable ? true : false,
                                             TimeoutOf(opt_timeout));
  const X_STATUS status = MapResult(result);
  if (status == X_STATUS_SUCCESS && wait.mutant) {
    MutantAcquired(*wait.mutant);
  }
  return status;
}

X_STATUS WaitOnMany(Runtime& runtime, std::span<const X_HANDLE> handles, u32 wait_type,
                    u32 wait_reason, u32 processor_mode, u32 alertable, u64* opt_timeout) {
  std::vector<WaitTarget> targets;
  targets.reserve(handles.size());
  std::vector<rex::platform::WaitHandle*> wait_handles;
  wait_handles.reserve(handles.size());
  for (const X_HANDLE handle : handles) {
    WaitTarget target = Resolve(runtime, handle);
    if (!target.slot) {
      return X_STATUS_INVALID_PARAMETER;
    }
    assert_not_null(target.handle);
    wait_handles.push_back(target.handle);
    targets.push_back(std::move(target));
  }
  const auto timeout = TimeoutOf(opt_timeout);
  ThreadRecord::CheckTitleTermination();
  if (wait_type) {
    auto result =
        rex::platform::WaitAny(std::move(wait_handles), alertable ? true : false, timeout);
    ThreadRecord::CheckTitleTermination();
    switch (result.first) {
      case rex::platform::WaitResult::kSuccess:
        if (targets[result.second].mutant) {
          MutantAcquired(*targets[result.second].mutant);
        }
        return X_STATUS(result.second);
      case rex::platform::WaitResult::kUserCallback:
        return X_STATUS_USER_APC;
      case rex::platform::WaitResult::kTimeout:
        rex::platform::MaybeYield();
        return X_STATUS_TIMEOUT;
      case rex::platform::WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
      default:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
    }
  }
  auto result = rex::platform::WaitAll(std::move(wait_handles), alertable ? true : false, timeout);
  ThreadRecord::CheckTitleTermination();
  switch (result) {
    case rex::platform::WaitResult::kSuccess:
      for (WaitTarget& target : targets) {
        if (target.mutant) {
          MutantAcquired(*target.mutant);
        }
      }
      return X_STATUS_SUCCESS;
    case rex::platform::WaitResult::kUserCallback:
      return X_STATUS_USER_APC;
    case rex::platform::WaitResult::kTimeout:
      rex::platform::MaybeYield();
      return X_STATUS_TIMEOUT;
    default:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

}  // namespace rex::runtime
