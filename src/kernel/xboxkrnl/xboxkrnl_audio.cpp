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

#include <rex/audio/audio_system.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

ppc_u32_result_t XAudioGetSpeakerConfig_entry(ppc_pu32_t config_ptr) {
  *config_ptr = 0x00010001;
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioGetVoiceCategoryVolumeChangeMask_entry(ppc_pvoid_t driver_ptr,
                                                              ppc_pu32_t out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  rex::thread::Sleep(std::chrono::microseconds(1));

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioGetVoiceCategoryVolume_entry(ppc_u32_t unk, ppc_pf32_t out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioEnableDucker_entry(ppc_u32_t unk) {
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioRegisterRenderDriverClient_entry(ppc_pu32_t callback_ptr,
                                                        ppc_pu32_t driver_ptr) {
  REXKRNL_DEBUG("XAudioRegisterRenderDriverClient called! callback_ptr={:08X} driver_ptr={:08X}",
                callback_ptr.guest_address(), driver_ptr.guest_address());
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioUnregisterRenderDriverClient_entry(ppc_pvoid_t driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioSubmitRenderDriverFrame_entry(ppc_pvoid_t driver_ptr,
                                                     ppc_pvoid_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  static uint32_t submit_krnl_count = 0;
  if (submit_krnl_count < 10) {
    REXKRNL_DEBUG("XAudioSubmitRenderDriverFrame: driver={:08X} samples={:08X}",
                  driver_ptr.guest_address(), samples_ptr.guest_address());
    submit_krnl_count++;
  }

  auto* audio_system =
      static_cast<audio::AudioSystem*>(REX_KERNEL_STATE()->emulator()->audio_system());
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples_ptr.guest_address());

  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

XBOXKRNL_EXPORT(__imp__XAudioGetSpeakerConfig, rex::kernel::xboxkrnl::XAudioGetSpeakerConfig_entry)
XBOXKRNL_EXPORT(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
                rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolumeChangeMask_entry)
XBOXKRNL_EXPORT(__imp__XAudioGetVoiceCategoryVolume,
                rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolume_entry)
XBOXKRNL_EXPORT(__imp__XAudioEnableDucker, rex::kernel::xboxkrnl::XAudioEnableDucker_entry)
XBOXKRNL_EXPORT(__imp__XAudioRegisterRenderDriverClient,
                rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry)
XBOXKRNL_EXPORT(__imp__XAudioUnregisterRenderDriverClient,
                rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry)
XBOXKRNL_EXPORT(__imp__XAudioSubmitRenderDriverFrame,
                rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry)

XBOXKRNL_EXPORT_STUB(__imp__XAudioRenderDriverInitialize);
XBOXKRNL_EXPORT_STUB(__imp__XAudioRenderDriverLock);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetVoiceCategoryVolume);
XBOXKRNL_EXPORT_STUB(__imp__XAudioBeginDigitalBypassMode);
XBOXKRNL_EXPORT_STUB(__imp__XAudioEndDigitalBypassMode);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSubmitDigitalPacket);
XBOXKRNL_EXPORT_STUB(__imp__XAudioQueryDriverPerformance);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetRenderDriverThread);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetSpeakerConfig);
XBOXKRNL_EXPORT_STUB(__imp__XAudioOverrideSpeakerConfig);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSuspendRenderDriverClients);
XBOXKRNL_EXPORT_STUB(__imp__XAudioRegisterRenderDriverMECClient);
XBOXKRNL_EXPORT_STUB(__imp__XAudioUnregisterRenderDriverMECClient);
XBOXKRNL_EXPORT_STUB(__imp__XAudioCaptureRenderDriverFrame);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetRenderDriverTic);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetDuckerLevel);
XBOXKRNL_EXPORT_STUB(__imp__XAudioIsDuckerEnabled);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetDuckerLevel);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetDuckerThreshold);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetDuckerThreshold);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetDuckerAttackTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetDuckerAttackTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetDuckerReleaseTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetDuckerReleaseTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetDuckerHoldTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetDuckerHoldTime);
XBOXKRNL_EXPORT_STUB(__imp__XAudioGetUnderrunCount);
XBOXKRNL_EXPORT_STUB(__imp__XAudioSetProcessFrameCallback);
