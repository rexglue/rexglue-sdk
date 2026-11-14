/**
 * @file        apu/sdl3/sdl3_audio_driver.h
 * @brief       The SDL3 audio driver and the rexapu-sdl3 provider (private to the module)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <vector>

#include <rex/apu/audio_driver.h>
#include <rex/driver_context.h>
#include <rex/driver_registration.h>
#include <rex/driver_module.h>

struct SDL_AudioStream;

namespace rex::apu::sdl3 {

/** Plays through one SDL3 audio stream bound to the default playback device. */
class SDL3AudioDriver final : public IAudioDriver {
 public:
  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Initialize(IDriverContext& context) override;
  void Shutdown() override;

  [[nodiscard]] Result<u32> OpenDevice(u32 channels, AudioFill fill) override;
  void CloseDevice() override;
  bool IsDeviceLost() const override;

  /** Runs on the SDL audio thread when the stream needs more data. */
  void Pull(SDL_AudioStream* stream, int additional_amount);

 private:
  IDriverContext* context_ = nullptr;
  SDL_AudioStream* stream_ = nullptr;
  u32 channels_ = 0;
  AudioFill fill_;
  std::vector<f32> scratch_;
};

/** The module that registers the SDL3 audio driver under the name "sdl3". */
class SDL3AudioProvider final : public IModule, public IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)

  std::string_view GetName() const override;
  [[nodiscard]] Result<void> Connect(IInterfaceFactory& factory) override;
  void Disconnect() override;
  [[nodiscard]] Result<void> Init() override;
  void Shutdown() override;
  IInterface* QueryInterface(std::string_view name, u32 version) override {
    return FindModuleInterface<IDriverModule>(*this, name, version);
  }
  std::span<const DriverRegistration> GetDrivers() const override;
};

}  // namespace rex::apu::sdl3
