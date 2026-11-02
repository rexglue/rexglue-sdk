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

#include <span>
#include <string>

#include <rex/apu/audio_driver.h>
#include <rex/platform.h>
#include <rex/module.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/settings.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::apu {

enum class AudioStreamID : u32 {};

/** One frame in host byte order, channels in the order FL, FR, C, LFE, SL, SR. */
using AudioFrame = std::span<const f32, kAudioFrameSamples>;

struct AudioStats {
  u32 open_streams = 0;
  u32 queued_frames = 0;
  u64 underruns = 0;  /**< Periods where an open stream had nothing queued. */
  u32 latency_us = 0; /**< Time from Submit to the device, averaged over the last second. */
};

class REX_APU_API AudioSettings final : public SettingOwner {
 public:
  AudioSettings();

  REX_SETTING(std::string, driver, "sdl3", "Audio driver, none for silence", kStartupOnly);
  REX_SETTING(bool, mute, false, "Mute the audio output");
  REX_SETTING(bool, mute_unfocused, false, "Mute while the window does not have focus");
  REX_SETTING(f64, gain, 1.0, "Output gain, 1 is unity", Range{0.0, 4.0});
  REX_SETTING(f64, center, 1.0, "Center level on a surround device", Range{0.0, 2.0});
  REX_SETTING(f64, surround, 1.0, "Surround level on a surround device", Range{0.0, 2.0});
  REX_SETTING(f64, lfe, 1.0, "LFE level on a surround device", Range{0.0, 2.0});
  REX_SETTING(f64, peak_limit, 0.0, "Silence a stream that stays above this peak, 0 is off",
              Range{0.0, 8.0});
  REX_SETTING(u32, max_queued_frames, 8, "Frames a stream may queue", Range{4, 64}, kUntilLaunch);
};

/**
 * For every period the device asks for, the mix takes one frame from every open
 * stream and applies the player's settings. A stream with nothing queued adds silence. With no
 * driver the mix runs on a timer, so every stream receives its frame played callbacks.
 *
 * A sample that is not finite becomes silence. With apu.peak_limit above 0, a stream whose peak
 * stays above it for two periods ramps to silence until a frame comes back under it.
 */
REX_DECLARE_INTERFACE(IAudioSystem, 1)
  static constexpr u32 kMaxStreams = 8;

  virtual AudioSettings& GetSettings() = 0;

  /**
   * @param on_frame_played Called on the mixing thread once for every frame the mix took from this
   *        stream. It must not block or call back into the system.
   *
   * @return The stream, or an error when kMaxStreams are open.
   */
  [[nodiscard]] virtual Result<AudioStreamID> OpenStream(
      MoveOnlyFunction<void()> on_frame_played) = 0;

  /**
   * Closes a stream. Queued frames are discarded. Returns after the stream's last callback.
   */
  virtual void CloseStream(AudioStreamID stream) = 0;

  /**
   * Queues a copy of a frame. Never waits for the device.
   *
   * @return Ok, or an error when the stream is unknown or holds max_queued_frames.
   */
  [[nodiscard]] virtual Result<void> Submit(AudioStreamID stream, AudioFrame frame) = 0;

  /**
   * Returns the channel count the device plays. It is fixed before any title launches. The count is
   * 6 or 2, and 2 with no driver.
   */
  virtual u32 GetOutputChannels() const = 0;

  /**
   * @return A snapshot.
   */
  virtual AudioStats GetStats() const = 0;

  /** Notifies the mix whether the window has focus. The UI system calls it. */
  virtual void SetFocused(bool focused) = 0;
};

}  // namespace rex::apu
