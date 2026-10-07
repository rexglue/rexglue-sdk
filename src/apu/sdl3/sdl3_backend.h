/**
 * @file        apu/sdl3/sdl3_backend.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>

#include <SDL3/SDL.h>

#include <rex/apu/backend.h>

namespace rex::apu::sdl3 {

/** The SDL3 audio output. 1 SDL audio stream on the default device per open stream. */
class SDL3Backend final : public IBackend, public NonCopyable {
 public:
  SDL3Backend();
  ~SDL3Backend() override;

  /**
   * Initializes the SDL audio subsystem.
   *
   * @return Ok, or the SDL error.
   */
  [[nodiscard]] Result<void> Init();

  /** Closes every stream and quits the SDL audio subsystem. */
  void Shutdown();

  [[nodiscard]] Result<u32> OpenStream(std::move_only_function<void()> on_frame_played) override;
  void CloseStream(u32 stream) override;
  void Submit(u32 stream, Frame frame) override;
  void SetStereoFold(const StereoFold& fold) override;
  StereoFold stereo_fold() const override;
  void SetSurroundMix(const SurroundMix& mix) override;
  SurroundMix surround_mix() const override;
  void SetOutputGain(f32 linear) override;
  f32 output_gain() const override;

 private:
  struct Stream {
    SDL3Backend* owner = nullptr;
    SDL_AudioStream* sdl = nullptr;
    u32 device_channels = 0;
    std::move_only_function<void()> on_frame_played;
    std::mutex frames_mutex;
    std::queue<std::unique_ptr<f32[]>> queued;
    std::stack<std::unique_ptr<f32[]>> unused;
  };

  static void Callback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                       int total_amount);
  [[nodiscard]] Result<SDL_AudioStream*> OpenDeviceStream(Stream& stream);

  bool initialized_ = false;
  std::mutex streams_mutex_;
  std::map<u32, std::unique_ptr<Stream>> streams_;
  u32 next_stream_ = 1;
  mutable std::mutex mix_mutex_;
  StereoFold fold_;
  SurroundMix mix_;
  f32 gain_ = 1.0f;
};

}  // namespace rex::apu::sdl3
