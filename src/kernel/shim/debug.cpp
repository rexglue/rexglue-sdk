/**
 * @file        kernel/shim/debug.cpp
 * @brief       Native debug output hook that replaces recompiled PPC
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cctype>
#include <string>

#include <rex/hook.h>
#include <rex/kernel/shim/function.h>
#include <rex/kernel/xboxkrnl/format.h>
#include <rex/logging/log.h>
#include <rex/runtime.h>
#include <rex/runtime/memory_view.h>
#include <rex/string.h>

namespace rex::kernel::shim {

namespace {

constexpr size_t kMaxFormatLength = 4096;

}  // namespace

REX_SHIM_FN_RAW(DebugPrint) {
  const u32 format_address = ctx.r3.u32;
  if (!rex::log::kernel.IsEnabled(rex::log::Level::kDebug)) {
    return;
  }
  Runtime* runtime = GetProcessRuntime();
  if (!runtime) {
    return;
  }
  const runtime::MemoryView view(*runtime->GetMemory());
  const auto format = view.TryGetCString(format_address, kMaxFormatLength);
  if (!format) {
    return;
  }
  const std::string format_text(*format);
  runtime::StringFormatData data(reinterpret_cast<const u8*>(format_text.c_str()));
  runtime::StackArgList args(ctx, base, 1);
  if (runtime::FormatCore(base, data, args, false) <= 0) {
    return;
  }
  std::string text = data.GetOutput();
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }
  if (text.empty()) {
    return;
  }
  REXLOG_DEBUG(kernel, "DebugPrint: {}", rex::ShiftJISToUTF8(text));
}

}  // namespace rex::kernel::shim
