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

#include <rex/api.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/runtime.h>

namespace rex {
namespace kernel {
namespace xam {

struct X_NUI_DEVICE_STATUS {
  be_u32 unk0;
  be_u32 unk1;
  be_u32 unk2;
  be_u32 status;
  be_u32 unk4;
  be_u32 unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

void XamNuiGetDeviceStatus_entry(GuestPtrT<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
  // 0 is not connected.
  status_ptr->status = 0;
}

u32 XamShowNuiTroubleshooterUI_entry([[maybe_unused]] u32 unk1, [[maybe_unused]] u32 unk2,
                                     [[maybe_unused]] u32 unk3) {
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamNuiGetDeviceStatus, rex::kernel::xam::XamNuiGetDeviceStatus_entry)
REX_EXPORT(__imp__XamShowNuiTroubleshooterUI, rex::kernel::xam::XamShowNuiTroubleshooterUI_entry)

REX_EXPORT_STUB(__imp__XamEnableNatalPlayback);
REX_EXPORT_STUB(__imp__XamEnableNuiAutomation);
REX_EXPORT_STUB(__imp__XamKinectGetHardwareType);
REX_EXPORT_STUB(__imp__XamNatalDeviceAudioCalibrate);
REX_EXPORT_STUB(__imp__XamNuiCameraAdjustTilt);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationAutoTilt);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationGetAngle);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationReverseAutoTilt);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationSetAngle);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationSetCallback);
REX_EXPORT_STUB(__imp__XamNuiCameraElevationStopMovement);
REX_EXPORT_STUB(__imp__XamNuiCameraGetTiltControllerType);
REX_EXPORT_STUB(__imp__XamNuiCameraRememberFloor);
REX_EXPORT_STUB(__imp__XamNuiCameraSetFlags);
REX_EXPORT_STUB(__imp__XamNuiCameraTiltGetStatus);
REX_EXPORT_STUB(__imp__XamNuiCameraTiltReportStatus);
REX_EXPORT_STUB(__imp__XamNuiCameraTiltSetCallback);
REX_EXPORT_STUB(__imp__XamNuiEnableChatMic);
REX_EXPORT_STUB(__imp__XamNuiGetCameraIntrinsics);
REX_EXPORT_STUB(__imp__XamNuiGetDepthCalibration);
REX_EXPORT_STUB(__imp__XamNuiGetDeviceSerialNumber);
REX_EXPORT_STUB(__imp__XamNuiGetFanRate);
REX_EXPORT_STUB(__imp__XamNuiGetLoadedDepthCalibration);
REX_EXPORT_STUB(__imp__XamNuiGetSupportString);
REX_EXPORT_STUB(__imp__XamNuiGetSystemGestureControl);
REX_EXPORT_STUB(__imp__XamNuiGetTrueColorInfo);
REX_EXPORT_STUB(__imp__XamNuiHudEnableInputFilter);
REX_EXPORT_STUB(__imp__XamNuiHudGetEngagedEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamNuiHudGetEngagedTrackingID);
REX_EXPORT_STUB(__imp__XamNuiHudGetInitializeFlags);
REX_EXPORT_STUB(__imp__XamNuiHudGetVersions);
REX_EXPORT_STUB(__imp__XamNuiHudInterpretFrame);
REX_EXPORT_STUB(__imp__XamNuiHudIsEnabled);
REX_EXPORT_STUB(__imp__XamNuiHudSetEngagedTrackingID);
REX_EXPORT_STUB(__imp__XamNuiIdentityAbort);
REX_EXPORT_STUB(__imp__XamNuiIdentityEnrollForSignIn);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetColorTexture);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetEnrollmentInfo);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlags);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlagsMessage);
REX_EXPORT_STUB(__imp__XamNuiIdentityGetSessionId);
REX_EXPORT_STUB(__imp__XamNuiIdentityIdentifyWithBiometric);
REX_EXPORT_STUB(__imp__XamNuiIdentityUnenroll);
REX_EXPORT_STUB(__imp__XamNuiIsChatMicEnabled);
REX_EXPORT_STUB(__imp__XamNuiIsDeviceReady);
REX_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateComplete);
REX_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateStarting);
REX_EXPORT_STUB(__imp__XamNuiPlayerEngagementUpdate);
REX_EXPORT_STUB(__imp__XamNuiSetForceDeviceOff);
REX_EXPORT_STUB(__imp__XamNuiSkeletonGetBestSkeletonIndex);
REX_EXPORT_STUB(__imp__XamNuiSkeletonScoreUpdate);
REX_EXPORT_STUB(__imp__XamNuiStoreDepthCalibration);
