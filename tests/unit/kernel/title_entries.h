/**
 * @file        tests/unit/kernel/title_entries.h
 * @brief       Guest entry functions for test titles, one that returns and two that block
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <initializer_list>
#include <string_view>

#include <rex/ppc/image.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/export.h>

#include "kernel/title_image.h"

namespace rex::test {

struct EntryCounts {
  std::atomic<int> entered{0};
  std::atomic<int> finished{0};
  std::atomic<u32> handle{0};
  std::atomic<bool> listening{false};
  std::atomic<int> devices_changed{0};

  void Reset() {
    entered = 0;
    finished = 0;
    handle = 0;
    listening = false;
    devices_changed = 0;
  }
};

inline EntryCounts g_entry;

inline u32 CallExport(PPCContext& ctx, u8* base, std::string_view name,
                      std::initializer_list<u64> args,
                      kernel::ExportModule module = kernel::ExportModule::kXboxkrnl) {
  PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6};
  size_t index = 0;
  for (const u64 value : args) {
    registers[index++]->u64 = value;
  }
  for (const auto& entry : kernel::GetExports(module)) {
    if (entry.name == name && entry.fn) {
      entry.fn(ctx, base);
      return ctx.r3.u32;
    }
  }
  return 0xFFFFFFFF;
}

inline void EntryReturns(PPCContext&, u8*) {
  ++g_entry.entered;
  ++g_entry.finished;
}

inline void EntryDelays(PPCContext& ctx, u8* base) {
  ++g_entry.entered;
  const u32 interval = ctx.r1.u32 - 0x80;
  *reinterpret_cast<be_u64*>(base + interval) = static_cast<u64>(-100000000000LL);
  CallExport(ctx, base, "KeDelayExecutionThread", {0, 1, interval});
  ++g_entry.finished;
}

inline void EntryWaitsOnEvent(PPCContext& ctx, u8* base) {
  ++g_entry.entered;
  const u32 handle_ptr = ctx.r1.u32 - 0x100;
  CallExport(ctx, base, "NtCreateEvent", {handle_ptr, 0, 0, 0});
  const u32 handle = *reinterpret_cast<be_u32*>(base + handle_ptr);
  g_entry.handle = handle;
  CallExport(ctx, base, "NtWaitForSingleObjectEx", {handle, 1, 1, 0});
  ++g_entry.finished;
}

inline void EntryListensForDevices(PPCContext& ctx, u8* base) {
  ++g_entry.entered;
  constexpr u32 kInputDevicesChanged = 0x00000012;
  const u32 id_ptr = ctx.r1.u32 - 0x100;
  const u32 param_ptr = ctx.r1.u32 - 0x110;
  const u32 interval = ctx.r1.u32 - 0x80;
  const u32 listener =
      CallExport(ctx, base, "XamNotifyCreateListener", {~0ull, 0}, kernel::ExportModule::kXAM);
  g_entry.listening = true;
  *reinterpret_cast<be_u64*>(base + interval) = static_cast<u64>(-10000);
  for (;;) {
    while (CallExport(ctx, base, "XNotifyGetNext", {listener, 0, id_ptr, param_ptr},
                      kernel::ExportModule::kXAM) == 1) {
      if (*reinterpret_cast<be_u32*>(base + id_ptr) == kInputDevicesChanged) {
        ++g_entry.devices_changed;
      }
    }
    CallExport(ctx, base, "KeDelayExecutionThread", {0, 1, interval});
  }
}

inline const PPCFuncMapping kReturningEntry[] = {{kTitleEntry, EntryReturns}, {0, nullptr}};
inline const PPCFuncMapping kDelayingEntry[] = {{kTitleEntry, EntryDelays}, {0, nullptr}};
inline const PPCFuncMapping kEventEntry[] = {{kTitleEntry, EntryWaitsOnEvent}, {0, nullptr}};
inline const PPCFuncMapping kListeningEntry[] = {{kTitleEntry, EntryListensForDevices},
                                                 {0, nullptr}};

}  // namespace rex::test
