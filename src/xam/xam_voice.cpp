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

#include "xam/xam_ui.h"
#include <rex/logging/log.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

u32 XamVoiceIsActiveProcess_entry() {
  // Returning 0 skips most of the voice handling.
  return 0;
}

u32 XamVoiceCreate_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                         MappedU32 out_voice_ptr) {
  out_voice_ptr.Zero();
  return kernel::X_ERROR_ACCESS_DENIED;
}

u32 XamVoiceClose_entry([[maybe_unused]] MappedVoid voice_ptr) {
  return 0;
}

u32 XamVoiceHeadsetPresent_entry([[maybe_unused]] MappedVoid voice_ptr) {
  return 0;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamVoiceIsActiveProcess, rex::xam::XamVoiceIsActiveProcess_entry)
REX_XAM_BODY(__imp__XamVoiceCreate, rex::xam::XamVoiceCreate_entry)
REX_XAM_BODY(__imp__XamVoiceClose, rex::xam::XamVoiceClose_entry)
REX_XAM_BODY(__imp__XamVoiceHeadsetPresent, rex::xam::XamVoiceHeadsetPresent_entry)
