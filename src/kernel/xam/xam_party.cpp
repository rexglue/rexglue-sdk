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
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {

ppc_u32_result_t XamPartyGetUserList_entry(ppc_u32_t player_count, ppc_pu32_t party_list) {
  // 5345085D wants specifically this code to skip loading party data.
  // This code is not documented in NT_STATUS code list
  return 0x807D0003;
}

ppc_u32_result_t XamPartySendGameInvites_entry(ppc_u32_t r3, ppc_u32_t r4, ppc_u32_t r5) {
  return X_ERROR_FUNCTION_FAILED;
}

ppc_u32_result_t XamPartySetCustomData_entry(ppc_u32_t r3, ppc_u32_t r4, ppc_u32_t r5) {
  return X_ERROR_FUNCTION_FAILED;
}

ppc_u32_result_t XamPartyGetBandwidth_entry(ppc_u32_t r3, ppc_u32_t r4) {
  return X_ERROR_FUNCTION_FAILED;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamPartyGetUserList, rex::kernel::xam::XamPartyGetUserList_entry)
XAM_EXPORT(__imp__XamPartySendGameInvites, rex::kernel::xam::XamPartySendGameInvites_entry)
XAM_EXPORT(__imp__XamPartySetCustomData, rex::kernel::xam::XamPartySetCustomData_entry)
XAM_EXPORT(__imp__XamPartyGetBandwidth, rex::kernel::xam::XamPartyGetBandwidth_entry)

XAM_EXPORT_STUB(__imp__XamPartyAddLocalUsers);
XAM_EXPORT_STUB(__imp__XamPartyAutomationInprocCall);
XAM_EXPORT_STUB(__imp__XamPartyCreate);
XAM_EXPORT_STUB(__imp__XamPartyGetAccessLevel);
XAM_EXPORT_STUB(__imp__XamPartyGetFormation);
XAM_EXPORT_STUB(__imp__XamPartyGetInfo);
XAM_EXPORT_STUB(__imp__XamPartyGetInfoEx);
XAM_EXPORT_STUB(__imp__XamPartyGetJoinable);
XAM_EXPORT_STUB(__imp__XamPartyGetNetworkCounters);
XAM_EXPORT_STUB(__imp__XamPartyGetRoutingTable);
XAM_EXPORT_STUB(__imp__XamPartyGetState);
XAM_EXPORT_STUB(__imp__XamPartyGetUserListInternal);
XAM_EXPORT_STUB(__imp__XamPartyIsCoordinator);
XAM_EXPORT_STUB(__imp__XamPartyJoin);
XAM_EXPORT_STUB(__imp__XamPartyJoinEx);
XAM_EXPORT_STUB(__imp__XamPartyKickUser);
XAM_EXPORT_STUB(__imp__XamPartyLeave);
XAM_EXPORT_STUB(__imp__XamPartyOverrideNatType);
XAM_EXPORT_STUB(__imp__XamPartyRemoveLocalUsers);
XAM_EXPORT_STUB(__imp__XamPartySendInvite);
XAM_EXPORT_STUB(__imp__XamPartySendInviteDeprecated);
XAM_EXPORT_STUB(__imp__XamPartySetConnectivityGraph);
XAM_EXPORT_STUB(__imp__XamPartySetJoinable);
XAM_EXPORT_STUB(__imp__XamPartySetTestDelay);
XAM_EXPORT_STUB(__imp__XamPartySetTestFlags);
