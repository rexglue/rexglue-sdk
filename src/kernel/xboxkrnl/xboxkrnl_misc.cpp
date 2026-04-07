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
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void KeEnableFpuExceptions_entry(u32 enabled) {
  // TODO(benvanik): can we do anything about exceptions?
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__KeEnableFpuExceptions, rex::kernel::xboxkrnl::KeEnableFpuExceptions_entry)

REX_EXPORT_STUB(__imp__ExSetBetaFeaturesEnabled);
REX_EXPORT_STUB(__imp__ExIsBetaFeatureEnabled);
REX_EXPORT_STUB(__imp__AniBlockOnAnimation);
REX_EXPORT_STUB(__imp__AniTerminateAnimation);
REX_EXPORT_STUB(__imp__AniSetLogo);
REX_EXPORT_STUB(__imp__AniStartBootAnimation);
REX_EXPORT_STUB(__imp__EtxConsumerDisableEventType);
REX_EXPORT_STUB(__imp__EtxConsumerEnableEventType);
REX_EXPORT_STUB(__imp__EtxConsumerProcessLogs);
REX_EXPORT_STUB(__imp__EtxConsumerRegister);
REX_EXPORT_STUB(__imp__EtxConsumerUnregister);
REX_EXPORT_STUB(__imp__EtxProducerLog);
REX_EXPORT_STUB(__imp__EtxProducerLogV);
REX_EXPORT_STUB(__imp__EtxProducerRegister);
REX_EXPORT_STUB(__imp__EtxProducerUnregister);
REX_EXPORT_STUB(__imp__EtxConsumerFlushBuffers);
REX_EXPORT_STUB(__imp__EtxProducerLogXwpp);
REX_EXPORT_STUB(__imp__EtxProducerLogXwppV);
REX_EXPORT_STUB(__imp__EtxBufferRegister);
REX_EXPORT_STUB(__imp__EtxBufferUnregister);
REX_EXPORT_STUB(__imp__KeEnablePPUPerformanceMonitor);
REX_EXPORT_STUB(__imp__KeEnterUserMode);
REX_EXPORT_STUB(__imp__KeLeaveUserMode);
REX_EXPORT_STUB(__imp__KeCreateUserMode);
REX_EXPORT_STUB(__imp__KeDeleteUserMode);
REX_EXPORT_STUB(__imp__KeEnablePFMInterrupt);
REX_EXPORT_STUB(__imp__KeDisablePFMInterrupt);
REX_EXPORT_STUB(__imp__KeSetProfilerISR);
REX_EXPORT_STUB(__imp__KeGetVidInfo);
REX_EXPORT_STUB(__imp__KeExecuteOnProtectedStack);
REX_EXPORT_STUB(__imp__EmaExecute);
REX_EXPORT_STUB(__imp__ExRegisterThreadNotification);
REX_EXPORT_STUB(__imp__ExTerminateTitleProcess);
REX_EXPORT_STUB(__imp__ExFreeDebugPool);
REX_EXPORT_STUB(__imp__ExReadModifyWriteXConfigSettingUlong);
REX_EXPORT_STUB(__imp__ExRegisterXConfigNotification);
REX_EXPORT_STUB(__imp__ExCancelAlarm);
REX_EXPORT_STUB(__imp__ExInitializeAlarm);
REX_EXPORT_STUB(__imp__ExSetAlarm);
REX_EXPORT_STUB(__imp__KeBlowFuses);
REX_EXPORT_STUB(__imp__KeGetPMWRegister);
REX_EXPORT_STUB(__imp__KeGetPRVRegister);
REX_EXPORT_STUB(__imp__KeGetSocRegister);
REX_EXPORT_STUB(__imp__KeGetSpecialPurposeRegister);
REX_EXPORT_STUB(__imp__KeSetPMWRegister);
REX_EXPORT_STUB(__imp__KeSetPowerMode);
REX_EXPORT_STUB(__imp__KeSetPRVRegister);
REX_EXPORT_STUB(__imp__KeSetSocRegister);
REX_EXPORT_STUB(__imp__KeSetSpecialPurposeRegister);
REX_EXPORT_STUB(__imp__KeCallAndBlockOnDpcRoutine);
REX_EXPORT_STUB(__imp__KeCallAndWaitForDpcRoutine);
REX_EXPORT_STUB(__imp__KeSetPageRelocationCallback);
REX_EXPORT_STUB(__imp__KeRegisterSwapNotification);

REX_EXPORT_STUB(__imp__DetroitDeviceRequest);
REX_EXPORT_STUB(__imp__IptvGetAesCtrTransform);
REX_EXPORT_STUB(__imp__IptvGetSessionKeyHash);
REX_EXPORT_STUB(__imp__IptvSetBoundaryKey);
REX_EXPORT_STUB(__imp__IptvSetSessionKey);
REX_EXPORT_STUB(__imp__IptvVerifyOmac1Signature);
REX_EXPORT_STUB(__imp__McaDeviceRequest);
REX_EXPORT_STUB(__imp__MicDeviceRequest);
REX_EXPORT_STUB(__imp__MtpdBeginTransaction);
REX_EXPORT_STUB(__imp__MtpdCancelTransaction);
REX_EXPORT_STUB(__imp__MtpdEndTransaction);
REX_EXPORT_STUB(__imp__MtpdGetCurrentDevices);
REX_EXPORT_STUB(__imp__MtpdReadData);
REX_EXPORT_STUB(__imp__MtpdReadEvent);
REX_EXPORT_STUB(__imp__MtpdResetDevice);
REX_EXPORT_STUB(__imp__MtpdSendData);
REX_EXPORT_STUB(__imp__MtpdVerifyProximity);
REX_EXPORT_STUB(__imp__NicAttach);
REX_EXPORT_STUB(__imp__NicDetach);
REX_EXPORT_STUB(__imp__NicFlushXmitQueue);
REX_EXPORT_STUB(__imp__NicGetLinkState);
REX_EXPORT_STUB(__imp__NicGetOpt);
REX_EXPORT_STUB(__imp__NicGetStats);
REX_EXPORT_STUB(__imp__NicRegisterDevice);
REX_EXPORT_STUB(__imp__NicSetOpt);
REX_EXPORT_STUB(__imp__NicSetUnicastAddress);
REX_EXPORT_STUB(__imp__NicShutdown);
REX_EXPORT_STUB(__imp__NicUnregisterDevice);
REX_EXPORT_STUB(__imp__NicUpdateMcastMembership);
REX_EXPORT_STUB(__imp__NicXmit);
REX_EXPORT_STUB(__imp__NomnilGetExtension);
REX_EXPORT_STUB(__imp__NomnilSetLed);
REX_EXPORT_STUB(__imp__NomnilStartCloseDevice);
REX_EXPORT_STUB(__imp__NullCableRequest);
REX_EXPORT_STUB(__imp__PsCamDeviceRequest);
REX_EXPORT_STUB(__imp__RmcDeviceRequest);
REX_EXPORT_STUB(__imp__TidDeviceRequest);
REX_EXPORT_STUB(__imp__TitleDeviceAuthRequest);
REX_EXPORT_STUB(__imp__UsbdAddDeviceComplete);
REX_EXPORT_STUB(__imp__UsbdCallAndBlockOnDpcRoutine);
REX_EXPORT_STUB(__imp__UsbdCancelAsyncTransfer);
REX_EXPORT_STUB(__imp__UsbdCancelTimer);
REX_EXPORT_STUB(__imp__UsbdEnableDisableRootHubPort);
REX_EXPORT_STUB(__imp__UsbdGetDeviceDescriptor);
REX_EXPORT_STUB(__imp__UsbdGetDeviceRootPortType);
REX_EXPORT_STUB(__imp__UsbdGetDeviceSpeed);
REX_EXPORT_STUB(__imp__UsbdGetDeviceTopology);
REX_EXPORT_STUB(__imp__UsbdGetEndpointDescriptor);
REX_EXPORT_STUB(__imp__UsbdGetNatalHardwareVersion);
REX_EXPORT_STUB(__imp__UsbdGetNatalHub);
REX_EXPORT_STUB(__imp__UsbdGetPortDeviceNode);
REX_EXPORT_STUB(__imp__UsbdGetRequiredDrivers);
REX_EXPORT_STUB(__imp__UsbdGetRootHubDeviceNode);
REX_EXPORT_STUB(__imp__UsbdIsDeviceAuthenticated);
REX_EXPORT_STUB(__imp__UsbdNatalHubRegisterNotificationCallback);
REX_EXPORT_STUB(__imp__UsbdOpenDefaultEndpoint);
REX_EXPORT_STUB(__imp__UsbdOpenEndpoint);
REX_EXPORT_STUB(__imp__UsbdQueueAsyncTransfer);
REX_EXPORT_STUB(__imp__UsbdQueueCloseDefaultEndpoint);
REX_EXPORT_STUB(__imp__UsbdQueueCloseEndpoint);
REX_EXPORT_STUB(__imp__UsbdQueueIsochTransfer);
REX_EXPORT_STUB(__imp__UsbdRegisterDriverObject);
REX_EXPORT_STUB(__imp__UsbdRemoveDeviceComplete);
REX_EXPORT_STUB(__imp__UsbdResetDevice);
REX_EXPORT_STUB(__imp__UsbdResetEndpoint);
REX_EXPORT_STUB(__imp__UsbdSetTimer);
REX_EXPORT_STUB(__imp__UsbdTitleDriverResetAllUnrecognizedPorts);
REX_EXPORT_STUB(__imp__UsbdTitleDriverSetUnrecognizedPort);
REX_EXPORT_STUB(__imp__UsbdUnregisterDriverObject);
REX_EXPORT_STUB(__imp__VeSetHandlers);
REX_EXPORT_STUB(__imp__VgcHandler_SetHandlers);
REX_EXPORT_STUB(__imp__VvcHandlerCancelTransfers);
REX_EXPORT_STUB(__imp__VvcHandlerRetrieveVoiceExtension);
REX_EXPORT_STUB(__imp__WifiBeginAuthentication);
REX_EXPORT_STUB(__imp__WifiCalculateRegulatoryDomain);
REX_EXPORT_STUB(__imp__WifiChannelToFrequency);
REX_EXPORT_STUB(__imp__WifiCheckCounterMeasures);
REX_EXPORT_STUB(__imp__WifiChooseAuthenCipherSetFromBSSID);
REX_EXPORT_STUB(__imp__WifiCompleteAuthentication);
REX_EXPORT_STUB(__imp__WifiDeduceNetworkType);
REX_EXPORT_STUB(__imp__WifiGetAssociationIE);
REX_EXPORT_STUB(__imp__WifiOnMICError);
REX_EXPORT_STUB(__imp__WifiPrepareAuthenticationContext);
REX_EXPORT_STUB(__imp__WifiRecvEAPOLPacket);
REX_EXPORT_STUB(__imp__WifiSelectAdHocChannel);
REX_EXPORT_STUB(__imp__XVoicedActivate);
REX_EXPORT_STUB(__imp__XVoicedClose);
REX_EXPORT_STUB(__imp__XVoicedGetBatteryStatus);
REX_EXPORT_STUB(__imp__XVoicedGetDirectionalData);
REX_EXPORT_STUB(__imp__XVoicedHeadsetPresent);
REX_EXPORT_STUB(__imp__XVoicedIsActiveProcess);
REX_EXPORT_STUB(__imp__XVoicedSendVPort);
REX_EXPORT_STUB(__imp__XVoicedSubmitPacket);
