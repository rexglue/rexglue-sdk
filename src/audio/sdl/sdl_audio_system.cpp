/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>

namespace rex::audio::sdl {

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(runtime::Processor* processor) {
  return std::make_unique<SDLAudioSystem>(processor);
}

SDLAudioSystem::SDLAudioSystem(runtime::Processor* processor) : AudioSystem(processor) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS SDLAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                      rex::thread::Semaphore* semaphore, AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new SDLAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto sdldriver = dynamic_cast<SDLAudioDriver*>(driver);
  assert_not_null(sdldriver);
  sdldriver->Shutdown();
  delete sdldriver;
}

}  // namespace rex::audio::sdl
