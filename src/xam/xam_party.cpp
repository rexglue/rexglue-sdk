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
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

u32 XamPartyGetUserList_entry([[maybe_unused]] u32 player_count,
                              [[maybe_unused]] MappedU32 party_list) {
  // 0x807D0003 makes the title skip loading party data.
  return 0x807D0003;
}

u32 XamPartySendGameInvites_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4,
                                  [[maybe_unused]] u32 r5) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

u32 XamPartySetCustomData_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4,
                                [[maybe_unused]] u32 r5) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

u32 XamPartyGetBandwidth_entry([[maybe_unused]] u32 r3, [[maybe_unused]] u32 r4) {
  return kernel::X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamPartyGetUserList, rex::xam::XamPartyGetUserList_entry)
REX_XAM_BODY(__imp__XamPartySendGameInvites, rex::xam::XamPartySendGameInvites_entry)
REX_XAM_BODY(__imp__XamPartySetCustomData, rex::xam::XamPartySetCustomData_entry)
REX_XAM_BODY(__imp__XamPartyGetBandwidth, rex::xam::XamPartyGetBandwidth_entry)
