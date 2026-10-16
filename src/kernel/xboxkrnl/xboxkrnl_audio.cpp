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

#include <rex/runtime/xthread.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/error.h>
#include <rex/runtime/xtypes.h>

#include "runtime/apu.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

u32 XAudioGetSpeakerConfig_entry(MappedU32 config_ptr) {
  *config_ptr = 0x00010001;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(MappedVoid driver_ptr, MappedU32 out_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  rex::runtime::KernelThread::Sleep(std::chrono::microseconds(1));

  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolume_entry([[maybe_unused]] u32 unk, MappedF32 out_ptr) {
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}

u32 XAudioEnableDucker_entry([[maybe_unused]] u32 unk) {
  return X_ERROR_SUCCESS;
}

u32 XAudioRegisterRenderDriverClient_entry(MappedU32 callback_ptr, MappedU32 driver_ptr) {
  REXLOG_DEBUG(kernel,
               "XAudioRegisterRenderDriverClient called! callback_ptr={:08X} driver_ptr={:08X}",
               callback_ptr.GetGuestAddress(), driver_ptr.GetGuestAddress());
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  u32 callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  u32 callback_arg = callback_ptr[1];

  runtime::APU* apu = runtime::GetCurrent().GetAPU();
  if (!apu) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto index = apu->RegisterClient(callback, callback_arg);
  if (!index) {
    return runtime::ToXStatus(index.error());
  }
  *driver_ptr = 0x41550000 | (*index & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioUnregisterRenderDriverClient_entry(MappedVoid driver_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  runtime::APU* apu = runtime::GetCurrent().GetAPU();
  if (!apu) {
    return X_ERROR_SUCCESS;
  }
  apu->UnregisterClient(driver_ptr.GetGuestAddress() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioSubmitRenderDriverFrame_entry(MappedVoid driver_ptr, MappedVoid samples_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  static u32 submit_krnl_count = 0;
  if (submit_krnl_count < 10) {
    REXLOG_DEBUG(kernel, "XAudioSubmitRenderDriverFrame: driver={:08X} samples={:08X}",
                 driver_ptr.GetGuestAddress(), samples_ptr.GetGuestAddress());
    submit_krnl_count++;
  }

  runtime::APU* apu = runtime::GetCurrent().GetAPU();
  if (!apu) {
    return X_ERROR_SUCCESS;
  }
  apu->SubmitFrame(driver_ptr.GetGuestAddress() & 0x0000FFFF, samples_ptr.GetGuestAddress());

  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__XAudioGetSpeakerConfig, rex::kernel::xboxkrnl::XAudioGetSpeakerConfig_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolumeChangeMask_entry)
REX_EXPORT(__imp__XAudioGetVoiceCategoryVolume,
           rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolume_entry)
REX_EXPORT(__imp__XAudioEnableDucker, rex::kernel::xboxkrnl::XAudioEnableDucker_entry)
REX_EXPORT(__imp__XAudioRegisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioUnregisterRenderDriverClient,
           rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry)
REX_EXPORT(__imp__XAudioSubmitRenderDriverFrame,
           rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry)

REX_EXPORT_STUB(__imp__XAudioRenderDriverInitialize);
REX_EXPORT_STUB(__imp__XAudioRenderDriverLock);
REX_EXPORT_STUB(__imp__XAudioSetVoiceCategoryVolume);
REX_EXPORT_STUB(__imp__XAudioBeginDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioEndDigitalBypassMode);
REX_EXPORT_STUB(__imp__XAudioSubmitDigitalPacket);
REX_EXPORT_STUB(__imp__XAudioQueryDriverPerformance);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverThread);
REX_EXPORT_STUB(__imp__XAudioSetSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioOverrideSpeakerConfig);
REX_EXPORT_STUB(__imp__XAudioSuspendRenderDriverClients);
REX_EXPORT_STUB(__imp__XAudioRegisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioUnregisterRenderDriverMECClient);
REX_EXPORT_STUB(__imp__XAudioCaptureRenderDriverFrame);
REX_EXPORT_STUB(__imp__XAudioGetRenderDriverTic);
REX_EXPORT_STUB(__imp__XAudioSetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioIsDuckerEnabled);
REX_EXPORT_STUB(__imp__XAudioGetDuckerLevel);
REX_EXPORT_STUB(__imp__XAudioGetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioSetDuckerThreshold);
REX_EXPORT_STUB(__imp__XAudioGetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerAttackTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerReleaseTime);
REX_EXPORT_STUB(__imp__XAudioGetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioSetDuckerHoldTime);
REX_EXPORT_STUB(__imp__XAudioGetUnderrunCount);
REX_EXPORT_STUB(__imp__XAudioSetProcessFrameCallback);
