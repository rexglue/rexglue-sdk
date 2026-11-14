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
#include <rex/logging/log_category.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

u32 XamAvatarInitialize_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                              [[maybe_unused]] u32 processor_number,
                              [[maybe_unused]] MappedU32 function_ptrs,
                              [[maybe_unused]] MappedVoid unk5, [[maybe_unused]] u32 unk6) {
  return ~0u;
}

void XamAvatarShutdown_entry() {}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamAvatarInitialize, rex::xam::XamAvatarInitialize_entry)
REX_XAM_BODY(__imp__XamAvatarShutdown, rex::xam::XamAvatarShutdown_entry)
