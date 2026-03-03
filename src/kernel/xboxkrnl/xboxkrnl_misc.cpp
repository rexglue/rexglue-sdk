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

XBOXKRNL_EXPORT_STUB(__imp__DetroitDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__IptvGetAesCtrTransform);
XBOXKRNL_EXPORT_STUB(__imp__IptvGetSessionKeyHash);
XBOXKRNL_EXPORT_STUB(__imp__IptvSetBoundaryKey);
XBOXKRNL_EXPORT_STUB(__imp__IptvSetSessionKey);
XBOXKRNL_EXPORT_STUB(__imp__IptvVerifyOmac1Signature);
XBOXKRNL_EXPORT_STUB(__imp__McaDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__MicDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__MtpdBeginTransaction);
XBOXKRNL_EXPORT_STUB(__imp__MtpdCancelTransaction);
XBOXKRNL_EXPORT_STUB(__imp__MtpdEndTransaction);
XBOXKRNL_EXPORT_STUB(__imp__MtpdGetCurrentDevices);
XBOXKRNL_EXPORT_STUB(__imp__MtpdReadData);
XBOXKRNL_EXPORT_STUB(__imp__MtpdReadEvent);
XBOXKRNL_EXPORT_STUB(__imp__MtpdResetDevice);
XBOXKRNL_EXPORT_STUB(__imp__MtpdSendData);
XBOXKRNL_EXPORT_STUB(__imp__MtpdVerifyProximity);
XBOXKRNL_EXPORT_STUB(__imp__NicAttach);
XBOXKRNL_EXPORT_STUB(__imp__NicDetach);
XBOXKRNL_EXPORT_STUB(__imp__NicFlushXmitQueue);
XBOXKRNL_EXPORT_STUB(__imp__NicGetLinkState);
XBOXKRNL_EXPORT_STUB(__imp__NicGetOpt);
XBOXKRNL_EXPORT_STUB(__imp__NicGetStats);
XBOXKRNL_EXPORT_STUB(__imp__NicRegisterDevice);
XBOXKRNL_EXPORT_STUB(__imp__NicSetOpt);
XBOXKRNL_EXPORT_STUB(__imp__NicSetUnicastAddress);
XBOXKRNL_EXPORT_STUB(__imp__NicShutdown);
XBOXKRNL_EXPORT_STUB(__imp__NicUnregisterDevice);
XBOXKRNL_EXPORT_STUB(__imp__NicUpdateMcastMembership);
XBOXKRNL_EXPORT_STUB(__imp__NicXmit);
XBOXKRNL_EXPORT_STUB(__imp__NomnilGetExtension);
XBOXKRNL_EXPORT_STUB(__imp__NomnilSetLed);
XBOXKRNL_EXPORT_STUB(__imp__NomnilStartCloseDevice);
XBOXKRNL_EXPORT_STUB(__imp__NullCableRequest);
XBOXKRNL_EXPORT_STUB(__imp__PsCamDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__RmcDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__TidDeviceRequest);
XBOXKRNL_EXPORT_STUB(__imp__TitleDeviceAuthRequest);
XBOXKRNL_EXPORT_STUB(__imp__UsbdAddDeviceComplete);
XBOXKRNL_EXPORT_STUB(__imp__UsbdCallAndBlockOnDpcRoutine);
XBOXKRNL_EXPORT_STUB(__imp__UsbdCancelAsyncTransfer);
XBOXKRNL_EXPORT_STUB(__imp__UsbdCancelTimer);
XBOXKRNL_EXPORT_STUB(__imp__UsbdEnableDisableRootHubPort);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetDeviceDescriptor);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetDeviceRootPortType);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetDeviceSpeed);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetDeviceTopology);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetEndpointDescriptor);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetNatalHardwareVersion);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetNatalHub);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetPortDeviceNode);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetRequiredDrivers);
XBOXKRNL_EXPORT_STUB(__imp__UsbdGetRootHubDeviceNode);
XBOXKRNL_EXPORT_STUB(__imp__UsbdIsDeviceAuthenticated);
XBOXKRNL_EXPORT_STUB(__imp__UsbdNatalHubRegisterNotificationCallback);
XBOXKRNL_EXPORT_STUB(__imp__UsbdOpenDefaultEndpoint);
XBOXKRNL_EXPORT_STUB(__imp__UsbdOpenEndpoint);
XBOXKRNL_EXPORT_STUB(__imp__UsbdQueueAsyncTransfer);
XBOXKRNL_EXPORT_STUB(__imp__UsbdQueueCloseDefaultEndpoint);
XBOXKRNL_EXPORT_STUB(__imp__UsbdQueueCloseEndpoint);
XBOXKRNL_EXPORT_STUB(__imp__UsbdQueueIsochTransfer);
XBOXKRNL_EXPORT_STUB(__imp__UsbdRegisterDriverObject);
XBOXKRNL_EXPORT_STUB(__imp__UsbdRemoveDeviceComplete);
XBOXKRNL_EXPORT_STUB(__imp__UsbdResetDevice);
XBOXKRNL_EXPORT_STUB(__imp__UsbdResetEndpoint);
XBOXKRNL_EXPORT_STUB(__imp__UsbdSetTimer);
XBOXKRNL_EXPORT_STUB(__imp__UsbdTitleDriverResetAllUnrecognizedPorts);
XBOXKRNL_EXPORT_STUB(__imp__UsbdTitleDriverSetUnrecognizedPort);
XBOXKRNL_EXPORT_STUB(__imp__UsbdUnregisterDriverObject);
XBOXKRNL_EXPORT_STUB(__imp__VeSetHandlers);
XBOXKRNL_EXPORT_STUB(__imp__VgcHandler_SetHandlers);
XBOXKRNL_EXPORT_STUB(__imp__VvcHandlerCancelTransfers);
XBOXKRNL_EXPORT_STUB(__imp__VvcHandlerRetrieveVoiceExtension);
XBOXKRNL_EXPORT_STUB(__imp__WifiBeginAuthentication);
XBOXKRNL_EXPORT_STUB(__imp__WifiCalculateRegulatoryDomain);
XBOXKRNL_EXPORT_STUB(__imp__WifiChannelToFrequency);
XBOXKRNL_EXPORT_STUB(__imp__WifiCheckCounterMeasures);
XBOXKRNL_EXPORT_STUB(__imp__WifiChooseAuthenCipherSetFromBSSID);
XBOXKRNL_EXPORT_STUB(__imp__WifiCompleteAuthentication);
XBOXKRNL_EXPORT_STUB(__imp__WifiDeduceNetworkType);
XBOXKRNL_EXPORT_STUB(__imp__WifiGetAssociationIE);
XBOXKRNL_EXPORT_STUB(__imp__WifiOnMICError);
XBOXKRNL_EXPORT_STUB(__imp__WifiPrepareAuthenticationContext);
XBOXKRNL_EXPORT_STUB(__imp__WifiRecvEAPOLPacket);
XBOXKRNL_EXPORT_STUB(__imp__WifiSelectAdHocChannel);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedActivate);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedClose);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedGetBatteryStatus);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedGetDirectionalData);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedHeadsetPresent);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedIsActiveProcess);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedSendVPort);
XBOXKRNL_EXPORT_STUB(__imp__XVoicedSubmitPacket);
