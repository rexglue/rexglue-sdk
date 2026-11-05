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

#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/logging/log.h>
#include "kernel/categories.h"
#include <rex/ppc/hook.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>

#include "kernel/apu.h"
#include <rex/kernel/thread_state.h>
#include "kernel/title_state.h"
#include "kernel/kernel.h"

namespace rex::kernel::xboxkrnl {
using namespace rex::kernel;

constexpr u32 kSpeakerAnalogSurround = 0x00000001;
constexpr u32 kSpeakerDigitalDolbyDigital = 0x00010000;
constexpr u32 kSurroundChannels = 6;

u32 XAudioGetSpeakerConfig_entry(MappedU32 config_ptr) {
  kernel::APU* apu = kernel::GetCurrentState().GetAPU();
  if (apu && apu->GetOutputChannels() == kSurroundChannels) {
    *config_ptr = kSpeakerDigitalDolbyDigital | kSpeakerAnalogSurround;
  } else {
    *config_ptr = kSpeakerAnalogSurround;
  }
  return X_ERROR_SUCCESS;
}

u32 XAudioGetVoiceCategoryVolumeChangeMask_entry(MappedVoid driver_ptr, MappedU32 out_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  rex::kernel::KernelThread::SleepFor(std::chrono::microseconds(1));

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
  REXKRNL_DEBUG("XAudioRegisterRenderDriverClient called! callback_ptr={:08X} driver_ptr={:08X}",
                callback_ptr.GetGuestAddress(), driver_ptr.GetGuestAddress());
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  u32 callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  u32 callback_arg = callback_ptr[1];

  kernel::APU* apu = kernel::GetCurrentState().GetAPU();
  if (!apu) {
    return X_STATUS_UNSUCCESSFUL;
  }
  auto index = apu->RegisterClient(callback, callback_arg);
  if (!index) {
    return kernel::ToXStatus(index.error());
  }
  *driver_ptr = 0x41550000 | (*index & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioUnregisterRenderDriverClient_entry(MappedVoid driver_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  kernel::APU* apu = kernel::GetCurrentState().GetAPU();
  if (!apu) {
    return X_ERROR_SUCCESS;
  }
  apu->UnregisterClient(driver_ptr.GetGuestAddress() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

u32 XAudioSubmitRenderDriverFrame_entry(MappedVoid driver_ptr, MappedVoid samples_ptr) {
  REX_ASSERT((driver_ptr.GetGuestAddress() & 0xFFFF0000) == 0x41550000);

  if (kernel::GetCurrentState().audio_submit_logs < 10) {
    REXKRNL_DEBUG("XAudioSubmitRenderDriverFrame: driver={:08X} samples={:08X}",
                  driver_ptr.GetGuestAddress(), samples_ptr.GetGuestAddress());
    ++kernel::GetCurrentState().audio_submit_logs;
  }

  kernel::APU* apu = kernel::GetCurrentState().GetAPU();
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
