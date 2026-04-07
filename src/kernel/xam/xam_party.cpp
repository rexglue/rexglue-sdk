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

#include <rex/kernel/xam/private.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {

u32 XamPartyGetUserList_entry(u32 player_count, mapped_u32 party_list) {
  // 5345085D wants specifically this code to skip loading party data.
  // This code is not documented in NT_STATUS code list
  return 0x807D0003;
}

u32 XamPartySendGameInvites_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartySetCustomData_entry(u32 r3, u32 r4, u32 r5) {
  return X_ERROR_FUNCTION_FAILED;
}

u32 XamPartyGetBandwidth_entry(u32 r3, u32 r4) {
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamPartyGetUserList, rex::kernel::xam::XamPartyGetUserList_entry)
REX_EXPORT(__imp__XamPartySendGameInvites, rex::kernel::xam::XamPartySendGameInvites_entry)
REX_EXPORT(__imp__XamPartySetCustomData, rex::kernel::xam::XamPartySetCustomData_entry)
REX_EXPORT(__imp__XamPartyGetBandwidth, rex::kernel::xam::XamPartyGetBandwidth_entry)

REX_EXPORT_STUB(__imp__XamPartyAddLocalUsers);
REX_EXPORT_STUB(__imp__XamPartyAutomationInprocCall);
REX_EXPORT_STUB(__imp__XamPartyCreate);
REX_EXPORT_STUB(__imp__XamPartyGetAccessLevel);
REX_EXPORT_STUB(__imp__XamPartyGetFormation);
REX_EXPORT_STUB(__imp__XamPartyGetInfo);
REX_EXPORT_STUB(__imp__XamPartyGetInfoEx);
REX_EXPORT_STUB(__imp__XamPartyGetJoinable);
REX_EXPORT_STUB(__imp__XamPartyGetNetworkCounters);
REX_EXPORT_STUB(__imp__XamPartyGetRoutingTable);
REX_EXPORT_STUB(__imp__XamPartyGetState);
REX_EXPORT_STUB(__imp__XamPartyGetUserListInternal);
REX_EXPORT_STUB(__imp__XamPartyIsCoordinator);
REX_EXPORT_STUB(__imp__XamPartyJoin);
REX_EXPORT_STUB(__imp__XamPartyJoinEx);
REX_EXPORT_STUB(__imp__XamPartyKickUser);
REX_EXPORT_STUB(__imp__XamPartyLeave);
REX_EXPORT_STUB(__imp__XamPartyOverrideNatType);
REX_EXPORT_STUB(__imp__XamPartyRemoveLocalUsers);
REX_EXPORT_STUB(__imp__XamPartySendInvite);
REX_EXPORT_STUB(__imp__XamPartySendInviteDeprecated);
REX_EXPORT_STUB(__imp__XamPartySetConnectivityGraph);
REX_EXPORT_STUB(__imp__XamPartySetJoinable);
REX_EXPORT_STUB(__imp__XamPartySetTestDelay);
REX_EXPORT_STUB(__imp__XamPartySetTestFlags);
