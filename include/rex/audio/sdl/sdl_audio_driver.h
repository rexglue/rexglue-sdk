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

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stack>
#include <string>
#include <unordered_map>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  uint32_t queued_frame_count() const override { return queued_count_.load(std::memory_order_relaxed); }
  uint32_t device_burst_frames() const override { return burst_frames_.load(std::memory_order_relaxed); }
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);

  // The output device (the audio_device setting, rex/audio/output_device.h).
  // OpenStream and CloseStream run with stream_mutex_ held.
  bool OpenStream(SDL_AudioDeviceID device);
  void CloseStream();
  void CheckDevice();
  void UpdateCurrentDevice();

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;

  static constexpr uint64_t kDeviceCheckIntervalMs = 2000;
  std::mutex stream_mutex_ = {};
  std::atomic<bool> have_stream_{false};
  SDL_AudioDeviceID opened_on_ = 0;  // the device decided on; 0 = the system default
  uint64_t next_device_check_ = 0;   // SDL_GetTicks() milliseconds
  // The system's default output at the last check (its listed name): a
  // default switched while playing is a new choice (the sound follows it).
  std::string last_default_;
  // When each connected device was first listed (SDL_GetTicks(); 0: before
  // the sound started), so a default that moves with a device plugged in
  // isn't taken for a choice.
  std::unordered_map<std::string, uint64_t> first_listed_;
  static constexpr uint64_t kSettledMs = 10000;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
  // frames_queued_.size(), and the most frames one device callback has taken
  // since the stream was opened (both kept under frames_mutex_, read freely).
  std::atomic<uint32_t> queued_count_{0};
  std::atomic<uint32_t> burst_frames_{0};
};

}  // namespace rex::audio::sdl
