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

#pragma once

#include <rex/audio/audio_system.h>

namespace rex::audio::nop {

class NopAudioSystem : public AudioSystem {
 public:
  explicit NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~NopAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::platform::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;
};

}  // namespace rex::audio::nop
