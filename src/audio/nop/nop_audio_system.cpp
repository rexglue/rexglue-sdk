/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/flags.h>
#include <rex/audio/nop/nop_audio_system.h>

namespace rex::audio::nop {

std::unique_ptr<AudioSystem> NopAudioSystem::Create(runtime::Processor* processor) {
  return std::make_unique<NopAudioSystem>(processor);
}

NopAudioSystem::NopAudioSystem(runtime::Processor* processor) : AudioSystem(processor) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  return X_STATUS_NOT_IMPLEMENTED;
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) {
  (void)driver;
  assert_always();
}

}  // namespace rex::audio::nop
