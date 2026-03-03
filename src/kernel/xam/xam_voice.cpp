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

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {

ppc_u32_result_t XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return 0;
}

ppc_u32_result_t XamVoiceCreate_entry(ppc_unknown_t unk1,  // 0
                                      ppc_unknown_t unk2,  // 0xF
                                      ppc_pu32_t out_voice_ptr) {
  // Null out the ptr.
  out_voice_ptr.Zero();
  return X_ERROR_ACCESS_DENIED;
}

ppc_u32_result_t XamVoiceClose_entry(ppc_pvoid_t voice_ptr) {
  return 0;
}

ppc_u32_result_t XamVoiceHeadsetPresent_entry(ppc_pvoid_t voice_ptr) {
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamVoiceIsActiveProcess, rex::kernel::xam::XamVoiceIsActiveProcess_entry)
XAM_EXPORT(__imp__XamVoiceCreate, rex::kernel::xam::XamVoiceCreate_entry)
XAM_EXPORT(__imp__XamVoiceClose, rex::kernel::xam::XamVoiceClose_entry)
XAM_EXPORT(__imp__XamVoiceHeadsetPresent, rex::kernel::xam::XamVoiceHeadsetPresent_entry)

XAM_EXPORT_STUB(__imp__XamMuteSound);
XAM_EXPORT_STUB(__imp__XamVoiceDisableMicArray);
XAM_EXPORT_STUB(__imp__XamVoiceGetBatteryStatus);
XAM_EXPORT_STUB(__imp__XamVoiceGetDirectionalData);
XAM_EXPORT_STUB(__imp__XamVoiceGetMicArrayAudio);
XAM_EXPORT_STUB(__imp__XamVoiceGetMicArrayAudioEx);
XAM_EXPORT_STUB(__imp__XamVoiceGetMicArrayFilenameDesc);
XAM_EXPORT_STUB(__imp__XamVoiceGetMicArrayStatus);
XAM_EXPORT_STUB(__imp__XamVoiceGetMicArrayUnderrunStatus);
XAM_EXPORT_STUB(__imp__XamVoiceMuteMicArray);
XAM_EXPORT_STUB(__imp__XamVoiceRecordUserPrivileges);
XAM_EXPORT_STUB(__imp__XamVoiceSetAudioCaptureRoutine);
XAM_EXPORT_STUB(__imp__XamVoiceSetMicArrayBeamAngle);
XAM_EXPORT_STUB(__imp__XamVoiceSetMicArrayIdleUsers);
XAM_EXPORT_STUB(__imp__XamVoiceSubmitPacket);
