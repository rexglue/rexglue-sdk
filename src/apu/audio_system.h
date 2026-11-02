/**
 * @file        apu/audio_system.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/apu/audio_driver.h>
#include <rex/apu/audio_system.h>

#include <rex/logging/log.h>

REXLOG_TABLE(kAudioLog, apu)

namespace rex::apu {

/** Mixes the open streams and feeds the device. The app starts and stops it. */
class AudioSystem final : public rex::apu::IAudioSystem {
 public:
  AudioSystem();
  ~AudioSystem() override;

  rex::apu::AudioSettings& GetSettings() override { return settings_; }
  [[nodiscard]] Result<rex::apu::AudioStreamID> OpenStream(
      MoveOnlyFunction<void()> on_frame_played) override;
  void CloseStream(rex::apu::AudioStreamID stream) override;
  [[nodiscard]] Result<void> Submit(rex::apu::AudioStreamID stream,
                                    rex::apu::AudioFrame frame) override;
  u32 GetOutputChannels() const override;
  rex::apu::AudioStats GetStats() const override;
  void SetFocused(bool focused) override;

  /**
   * Starts the mix thread.
   *
   * @param driver The device, or null to run the mix on a timer.
   * @return Success, or the reason the mix cannot start.
   */
  [[nodiscard]] Result<void> Start(rex::apu::IAudioDriver* driver);

  /** Stops the mix and closes the device. */
  void Stop();

 private:
  rex::apu::AudioSettings settings_;
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex::apu
