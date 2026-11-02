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

#include <format>

#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_debug_output.h>
#include <rex/kernel/title_runtime.h>
#include "xam/xam_bodies.h"
#include "xam/xam_state.h"

namespace rex {
namespace xam {

void OutputDebugStringA_entry(MappedString string) {
  if (string) {
    CurrentTitle().debug.WriteTitleDebugString(
        std::format("OutputDebugStringA: {}", string.GetValue()));
  }
}

void OutputDebugStringW_entry(MappedWString string) {
  if (string) {
    std::u16string_view sv = string.GetValue();
    std::string utf8;
    utf8.reserve(sv.size());
    for (char16_t c : sv) {
      if (c < 0x80) {
        utf8.push_back(static_cast<char>(c));
      } else {
        utf8.push_back('?');
      }
    }
    CurrentTitle().debug.WriteTitleDebugString(std::format("OutputDebugStringW: {}", utf8));
  }
}

void RtlOutputDebugString_entry(MappedString string) {
  if (string) {
    CurrentTitle().debug.WriteTitleDebugString(
        std::format("RtlOutputDebugString: {}", string.GetValue()));
  }
}

void RtlDebugTrace_entry(MappedString string) {
  if (string) {
    CurrentTitle().debug.WriteTitleDebugString(std::format("RtlDebugTrace: {}", string.GetValue()));
  }
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__OutputDebugStringA, rex::xam::OutputDebugStringA_entry)
REX_XAM_BODY(__imp__OutputDebugStringW, rex::xam::OutputDebugStringW_entry)
REX_XAM_BODY(__imp__RtlOutputDebugString, rex::xam::RtlOutputDebugString_entry)
REX_XAM_BODY(__imp__RtlDebugTrace, rex::xam::RtlDebugTrace_entry)
