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

#include <string>

#include <rex/memory/endian.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_debug_output.h>
#include <rex/kernel/title_runtime.h>
#include <rex/string/text.h>
#include "xam/xam_bodies.h"
#include "xam/xam_state.h"

namespace rex {
namespace xam {

namespace {

void WriteNarrow(MappedString string) {
  rex::kernel::ITitleDebugOutput& debug = CurrentTitle().debug;
  if (string && debug.IsEnabled()) {
    debug.Write(string.GetValue(), debug.GetEncoding());
  }
}

}  // namespace

void OutputDebugStringA_entry(MappedString string) {
  WriteNarrow(string);
}

void OutputDebugStringW_entry(MappedWString string) {
  rex::kernel::ITitleDebugOutput& debug = CurrentTitle().debug;
  if (string && debug.IsEnabled()) {
    const auto text = rex::memory::load_and_swap<std::u16string>(string.GetValue().data());
    debug.Write(rex::string::ToUTF8(text), rex::kernel::TextEncoding::kUTF8);
  }
}

void RtlOutputDebugString_entry(MappedString string) {
  WriteNarrow(string);
}

void RtlDebugTrace_entry(MappedString string) {
  WriteNarrow(string);
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__OutputDebugStringA, rex::xam::OutputDebugStringA_entry)
REX_XAM_BODY(__imp__OutputDebugStringW, rex::xam::OutputDebugStringW_entry)
REX_XAM_BODY(__imp__RtlOutputDebugString, rex::xam::RtlOutputDebugString_entry)
REX_XAM_BODY(__imp__RtlDebugTrace, rex::xam::RtlDebugTrace_entry)
