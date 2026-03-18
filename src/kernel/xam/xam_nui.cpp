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

#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

extern std::atomic<int> xam_dialogs_shown_;

struct X_NUI_DEVICE_STATUS {
  rex::be<uint32_t> unk0;
  rex::be<uint32_t> unk1;
  rex::be<uint32_t> unk2;
  rex::be<uint32_t> status;
  rex::be<uint32_t> unk4;
  rex::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

void XamNuiGetDeviceStatus_entry(ppc_ptr_t<X_NUI_DEVICE_STATUS> status_ptr) {
  status_ptr.Zero();
  status_ptr->status = 0;  // Not connected.
}

ppc_u32_result_t XamShowNuiTroubleshooterUI_entry(ppc_unknown_t unk1, ppc_unknown_t unk2,
                                                  ppc_unknown_t unk3) {
  // unk1 is 0xFF - possibly user index?
  // unk2, unk3 appear to always be zero.

  if (REXCVAR_GET(headless)) {
    return 0;
  }
  // TODO(tomc): Implement imgui stuff
  // const Runtime* emulator = REX_KERNEL_STATE()->emulator();
  // ui::Window* display_window = emulator->display_window();
  // ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  // if (display_window && imgui_drawer) {
  //  rex::thread::Fence fence;
  //  if (display_window->app_context().CallInUIThreadSynchronous([&]() {
  //        rex::ui::ImGuiDialog::ShowMessageBox(
  //            imgui_drawer, "NUI Troubleshooter",
  //            "The game has indicated there is a problem with NUI (Kinect).")
  //            ->Then(&fence);
  //      })) {
  //    ++xam_dialogs_shown_;
  //    fence.Wait();
  //    --xam_dialogs_shown_;
  //  }
  //}

  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamNuiGetDeviceStatus, rex::kernel::xam::XamNuiGetDeviceStatus_entry)
XAM_EXPORT(__imp__XamShowNuiTroubleshooterUI, rex::kernel::xam::XamShowNuiTroubleshooterUI_entry)

XAM_EXPORT_STUB(__imp__XamEnableNatalPlayback);
XAM_EXPORT_STUB(__imp__XamEnableNuiAutomation);
XAM_EXPORT_STUB(__imp__XamKinectGetHardwareType);
XAM_EXPORT_STUB(__imp__XamNatalDeviceAudioCalibrate);
XAM_EXPORT_STUB(__imp__XamNuiCameraAdjustTilt);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationAutoTilt);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationGetAngle);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationReverseAutoTilt);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationSetAngle);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationSetCallback);
XAM_EXPORT_STUB(__imp__XamNuiCameraElevationStopMovement);
XAM_EXPORT_STUB(__imp__XamNuiCameraGetTiltControllerType);
XAM_EXPORT_STUB(__imp__XamNuiCameraRememberFloor);
XAM_EXPORT_STUB(__imp__XamNuiCameraSetFlags);
XAM_EXPORT_STUB(__imp__XamNuiCameraTiltGetStatus);
XAM_EXPORT_STUB(__imp__XamNuiCameraTiltReportStatus);
XAM_EXPORT_STUB(__imp__XamNuiCameraTiltSetCallback);
XAM_EXPORT_STUB(__imp__XamNuiEnableChatMic);
XAM_EXPORT_STUB(__imp__XamNuiGetCameraIntrinsics);
XAM_EXPORT_STUB(__imp__XamNuiGetDepthCalibration);
XAM_EXPORT_STUB(__imp__XamNuiGetDeviceSerialNumber);
XAM_EXPORT_STUB(__imp__XamNuiGetFanRate);
XAM_EXPORT_STUB(__imp__XamNuiGetLoadedDepthCalibration);
XAM_EXPORT_STUB(__imp__XamNuiGetSupportString);
XAM_EXPORT_STUB(__imp__XamNuiGetSystemGestureControl);
XAM_EXPORT_STUB(__imp__XamNuiGetTrueColorInfo);
XAM_EXPORT_STUB(__imp__XamNuiHudEnableInputFilter);
XAM_EXPORT_STUB(__imp__XamNuiHudGetEngagedEnrollmentIndex);
XAM_EXPORT_STUB(__imp__XamNuiHudGetEngagedTrackingID);
XAM_EXPORT_STUB(__imp__XamNuiHudGetInitializeFlags);
XAM_EXPORT_STUB(__imp__XamNuiHudGetVersions);
XAM_EXPORT_STUB(__imp__XamNuiHudInterpretFrame);
XAM_EXPORT_STUB(__imp__XamNuiHudIsEnabled);
XAM_EXPORT_STUB(__imp__XamNuiHudSetEngagedTrackingID);
XAM_EXPORT_STUB(__imp__XamNuiIdentityAbort);
XAM_EXPORT_STUB(__imp__XamNuiIdentityEnrollForSignIn);
XAM_EXPORT_STUB(__imp__XamNuiIdentityGetColorTexture);
XAM_EXPORT_STUB(__imp__XamNuiIdentityGetEnrollmentInfo);
XAM_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlags);
XAM_EXPORT_STUB(__imp__XamNuiIdentityGetQualityFlagsMessage);
XAM_EXPORT_STUB(__imp__XamNuiIdentityGetSessionId);
XAM_EXPORT_STUB(__imp__XamNuiIdentityIdentifyWithBiometric);
XAM_EXPORT_STUB(__imp__XamNuiIdentityUnenroll);
XAM_EXPORT_STUB(__imp__XamNuiIsChatMicEnabled);
XAM_EXPORT_STUB(__imp__XamNuiIsDeviceReady);
XAM_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateComplete);
XAM_EXPORT_STUB(__imp__XamNuiNatalCameraUpdateStarting);
XAM_EXPORT_STUB(__imp__XamNuiPlayerEngagementUpdate);
XAM_EXPORT_STUB(__imp__XamNuiSetForceDeviceOff);
XAM_EXPORT_STUB(__imp__XamNuiSkeletonGetBestSkeletonIndex);
XAM_EXPORT_STUB(__imp__XamNuiSkeletonScoreUpdate);
XAM_EXPORT_STUB(__imp__XamNuiStoreDepthCalibration);
