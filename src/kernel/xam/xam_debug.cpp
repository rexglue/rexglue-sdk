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

#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xam {

void OutputDebugStringA_entry(mapped_string string) {
  if (string) {
    REXLOG_DEV(title, "OutputDebugStringA: {}", string.value());
  }
}

void OutputDebugStringW_entry(mapped_wstring string) {
  if (string) {
    std::u16string_view sv = string.value();
    std::string utf8;
    utf8.reserve(sv.size());
    for (char16_t c : sv) {
      if (c < 0x80) {
        utf8.push_back(static_cast<char>(c));
      } else {
        utf8.push_back('?');
      }
    }
    REXLOG_DEV(title, "OutputDebugStringW: {}", utf8);
  }
}

void RtlOutputDebugString_entry(mapped_string string) {
  if (string) {
    REXLOG_DEV(title, "RtlOutputDebugString: {}", string.value());
  }
}

void RtlDebugTrace_entry(mapped_string string) {
  if (string) {
    REXLOG_DEV(title, "RtlDebugTrace: {}", string.value());
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__OutputDebugStringA, rex::kernel::xam::OutputDebugStringA_entry)
REX_EXPORT(__imp__OutputDebugStringW, rex::kernel::xam::OutputDebugStringW_entry)
REX_EXPORT(__imp__RtlOutputDebugString, rex::kernel::xam::RtlOutputDebugString_entry)
REX_EXPORT(__imp__RtlDebugTrace, rex::kernel::xam::RtlDebugTrace_entry)
