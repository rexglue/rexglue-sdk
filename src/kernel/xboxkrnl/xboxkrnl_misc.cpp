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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void KeEnableFpuExceptions_entry(ppc_u32_t enabled) {
  // TODO(benvanik): can we do anything about exceptions?
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__KeEnableFpuExceptions, rex::kernel::xboxkrnl::KeEnableFpuExceptions_entry)

XBOXKRNL_EXPORT_STUB(__imp__ExSetBetaFeaturesEnabled);
XBOXKRNL_EXPORT_STUB(__imp__ExIsBetaFeatureEnabled);
XBOXKRNL_EXPORT_STUB(__imp__AniBlockOnAnimation);
XBOXKRNL_EXPORT_STUB(__imp__AniTerminateAnimation);
XBOXKRNL_EXPORT_STUB(__imp__AniSetLogo);
XBOXKRNL_EXPORT_STUB(__imp__AniStartBootAnimation);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerDisableEventType);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerEnableEventType);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerProcessLogs);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerRegister);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerUnregister);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerLog);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerLogV);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerRegister);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerUnregister);
XBOXKRNL_EXPORT_STUB(__imp__EtxConsumerFlushBuffers);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerLogXwpp);
XBOXKRNL_EXPORT_STUB(__imp__EtxProducerLogXwppV);
XBOXKRNL_EXPORT_STUB(__imp__EtxBufferRegister);
XBOXKRNL_EXPORT_STUB(__imp__EtxBufferUnregister);
XBOXKRNL_EXPORT_STUB(__imp__KeEnablePPUPerformanceMonitor);
XBOXKRNL_EXPORT_STUB(__imp__KeEnterUserMode);
XBOXKRNL_EXPORT_STUB(__imp__KeLeaveUserMode);
XBOXKRNL_EXPORT_STUB(__imp__KeCreateUserMode);
XBOXKRNL_EXPORT_STUB(__imp__KeDeleteUserMode);
XBOXKRNL_EXPORT_STUB(__imp__KeEnablePFMInterrupt);
XBOXKRNL_EXPORT_STUB(__imp__KeDisablePFMInterrupt);
XBOXKRNL_EXPORT_STUB(__imp__KeSetProfilerISR);
XBOXKRNL_EXPORT_STUB(__imp__KeGetVidInfo);
XBOXKRNL_EXPORT_STUB(__imp__KeExecuteOnProtectedStack);
XBOXKRNL_EXPORT_STUB(__imp__EmaExecute);
XBOXKRNL_EXPORT_STUB(__imp__ExRegisterThreadNotification);
XBOXKRNL_EXPORT_STUB(__imp__ExTerminateTitleProcess);
XBOXKRNL_EXPORT_STUB(__imp__ExFreeDebugPool);
XBOXKRNL_EXPORT_STUB(__imp__ExReadModifyWriteXConfigSettingUlong);
XBOXKRNL_EXPORT_STUB(__imp__ExRegisterXConfigNotification);
XBOXKRNL_EXPORT_STUB(__imp__ExCancelAlarm);
XBOXKRNL_EXPORT_STUB(__imp__ExInitializeAlarm);
XBOXKRNL_EXPORT_STUB(__imp__ExSetAlarm);
XBOXKRNL_EXPORT_STUB(__imp__KeBlowFuses);
XBOXKRNL_EXPORT_STUB(__imp__KeGetPMWRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeGetPRVRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeGetSocRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeGetSpecialPurposeRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPMWRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPowerMode);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPRVRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeSetSocRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeSetSpecialPurposeRegister);
XBOXKRNL_EXPORT_STUB(__imp__KeCallAndBlockOnDpcRoutine);
XBOXKRNL_EXPORT_STUB(__imp__KeCallAndWaitForDpcRoutine);
XBOXKRNL_EXPORT_STUB(__imp__KeSetPageRelocationCallback);
XBOXKRNL_EXPORT_STUB(__imp__KeRegisterSwapNotification);
