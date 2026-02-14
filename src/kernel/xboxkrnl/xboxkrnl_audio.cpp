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

  rex::thread::MaybeYield();

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
  uint32_t callback = callback_ptr[0];
  uint32_t callback_arg = callback_ptr[1];

  auto* audio_system = static_cast<audio::AudioSystem*>(kernel_state()->emulator()->audio_system());

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

  auto* audio_system = static_cast<audio::AudioSystem*>(kernel_state()->emulator()->audio_system());
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}

ppc_u32_result_t XAudioSubmitRenderDriverFrame_entry(ppc_pvoid_t driver_ptr,
                                                     ppc_pvoid_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto* audio_system = static_cast<audio::AudioSystem*>(kernel_state()->emulator()->audio_system());
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples_ptr.guest_address());

  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

PPC_HOOK(__imp__XAudioGetSpeakerConfig, rex::kernel::xboxkrnl::XAudioGetSpeakerConfig_entry)
PPC_HOOK(__imp__XAudioGetVoiceCategoryVolumeChangeMask,
         rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolumeChangeMask_entry)
PPC_HOOK(__imp__XAudioGetVoiceCategoryVolume,
         rex::kernel::xboxkrnl::XAudioGetVoiceCategoryVolume_entry)
PPC_HOOK(__imp__XAudioEnableDucker, rex::kernel::xboxkrnl::XAudioEnableDucker_entry)
PPC_HOOK(__imp__XAudioRegisterRenderDriverClient,
         rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry)
PPC_HOOK(__imp__XAudioUnregisterRenderDriverClient,
         rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry)
PPC_HOOK(__imp__XAudioSubmitRenderDriverFrame,
         rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry)
