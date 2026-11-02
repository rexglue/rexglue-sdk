/**
 * @file        apu/audio_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <span>

#include <rex/driver.h>
#include <rex/logging/log.h>
#include <rex/memory/host.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::apu {

/** The tag of audio emulation and the audio drivers. The audio provider names it "APU". */
inline constexpr memory::Tag kAPU{4};

/** A submitted frame holds 6 channels one after another, 256 samples each at 48 kHz. */
inline constexpr u32 kAudioFrameChannels = 6;
inline constexpr u32 kAudioChannelSamples = 256;
inline constexpr u32 kAudioSampleRate = 48000;
inline constexpr u32 kAudioFrameSamples = kAudioFrameChannels * kAudioChannelSamples;

/**
 * Fills the samples the device is about to play. The device calls it from its own thread.
 *
 * @param samples Interleaved 48 kHz f32 in host byte order, written in full.
 */
using AudioFill = MoveOnlyFunction<void(std::span<f32> samples, u32 channels)>;

/** An audio device that pulls samples through its AudioFill. The mix thread makes the calls. */
class IAudioDriver : public IDriver {
 public:
  /**
   * Opens the default output device and starts it playing.
   *
   * @param channels The channel count wanted, 6 or 2.
   * @param fill Called for every buffer the device needs. It stays valid until CloseDevice returns.
   * @return The channel count the device plays, 6 or 2, or an error when no device opens.
   */
  [[nodiscard]] virtual Result<u32> OpenDevice(u32 channels, AudioFill fill) = 0;

  /** Closes the device. Returns once no call to the fill is running and none starts again. */
  virtual void CloseDevice() = 0;

  virtual bool IsDeviceLost() const = 0;
};

}  // namespace rex::apu
