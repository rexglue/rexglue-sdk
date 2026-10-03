/**
 * @file        apu/backend.h
 * @brief       The audio output interface served by apu modules and its null provider
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <thread>

#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::apu {

/** Channels in 1 guest audio frame. */
inline constexpr u32 kFrameChannels = 6;
/** Samples per channel in 1 guest audio frame. */
inline constexpr u32 kChannelSamples = 256;
/** Sample rate of the title audio frames in hertz. */
inline constexpr u32 kFrameRate = 48000;
/** Samples in 1 guest audio frame. */
inline constexpr u32 kFrameSamples = kFrameChannels * kChannelSamples;

/** 1 guest audio frame, 6 sequential channels of 256 big endian samples in the order fl fr fc lf bl
 * br. */
using Frame = std::span<const be_f32, kFrameSamples>;

/**
 * Weights applied when a 5.1 frame is folded to a stereo device. The front channels carry an
 * implicit weight of 1. Defaults follow ITU-R BS.775 with LFE dropped.
 */
struct StereoFold {
  /** Center weight. */
  f32 center = 0.70710678f;
  /** Surround weight. */
  f32 surround = 0.70710678f;
  /** LFE weight. */
  f32 lfe = 0.0f;
  /** Output scale, 1/(1+0.707) by default. */
  f32 scale = 0.58578644f;
};

/** Weights applied when a 5.1 frame reaches a device with more than 2 channels. Unity passes
 * through. */
struct SurroundMix {
  /** Center weight. */
  f32 center = 1.0f;
  /** Surround weight. */
  f32 surround = 1.0f;
  /** LFE weight. */
  f32 lfe = 1.0f;
};

/** The apu config, read by the runtime. */
struct APUConfig {
  /** Default of apu.max_queued_frames, 4 to 64. */
  u32 max_queued_frames = 8;
};

/** An audio output, served by an apu module. Every method is safe on any thread. */
class IBackend {
 public:
  REX_INTERFACE(IBackend, "rex.apu.backend", 1);

  virtual ~IBackend() = default;

  /**
   * Opens an output stream. on_frame_played runs on a backend thread and must not call into the
   * backend.
   *
   * @param on_frame_played Called once per submitted frame after the frame leaves the queue.
   * @return The stream id, or the backend failure.
   */
  [[nodiscard]] virtual Result<u32> OpenStream(std::move_only_function<void()> on_frame_played) = 0;

  /**
   * Closes a stream. Returns after the last on_frame_played of the stream has returned. Queued
   * frames are dropped without a callback.
   *
   * @param stream A stream id from OpenStream.
   */
  virtual void CloseStream(u32 stream) = 0;

  /**
   * Queues a frame. The frame is copied before Submit returns.
   *
   * @param stream A stream id from OpenStream.
   * @param frame The frame.
   */
  virtual void Submit(u32 stream, Frame frame) = 0;

  /**
   * Sets the stereo fold, picked up on the next device period.
   *
   * @param fold The weights.
   */
  virtual void SetStereoFold(const StereoFold& fold) = 0;

  /**
   * Returns the stereo fold.
   *
   * @return The weights.
   */
  virtual StereoFold stereo_fold() const = 0;

  /**
   * Sets the surround mix, picked up on the next device period.
   *
   * @param mix The weights.
   */
  virtual void SetSurroundMix(const SurroundMix& mix) = 0;

  /**
   * Returns the surround mix.
   *
   * @return The weights.
   */
  virtual SurroundMix surround_mix() const = 0;

  /**
   * Sets the linear output gain, picked up on the next device period. Output is clamped.
   *
   * @param linear The gain, 1 is unity.
   */
  virtual void SetOutputGain(f32 linear) = 0;

  /**
   * Returns the linear output gain.
   *
   * @return The gain.
   */
  virtual f32 output_gain() const = 0;
};

/** The null audio output. A clock thread plays 1 queued frame per stream every frame period. */
class REX_API NullBackend final : public IBackend, public NonCopyable {
 public:
  /** Starts the "APU Null Clock" thread. */
  NullBackend();

  /** Stops the clock thread. */
  ~NullBackend() override;

  /**
   * Opens a stream the clock thread plays.
   *
   * @param on_frame_played Called once per frame the clock plays.
   * @return The stream id.
   */
  [[nodiscard]] Result<u32> OpenStream(std::move_only_function<void()> on_frame_played) override;

  /**
   * Closes a stream and drops its queued frames.
   *
   * @param stream A stream id from OpenStream.
   */
  void CloseStream(u32 stream) override;

  /**
   * Queues a frame. The samples are discarded.
   *
   * @param stream A stream id from OpenStream.
   * @param frame The frame.
   */
  void Submit(u32 stream, Frame frame) override;

  /**
   * Stores the stereo fold.
   *
   * @param fold The fold.
   */
  void SetStereoFold(const StereoFold& fold) override;

  /**
   * Returns the stored stereo fold.
   *
   * @return The fold.
   */
  StereoFold stereo_fold() const override;

  /**
   * Stores the surround mix.
   *
   * @param mix The mix.
   */
  void SetSurroundMix(const SurroundMix& mix) override;

  /**
   * Returns the stored surround mix.
   *
   * @return The mix.
   */
  SurroundMix surround_mix() const override;

  /**
   * Stores the output gain.
   *
   * @param linear The gain, 1 is unity.
   */
  void SetOutputGain(f32 linear) override;

  /**
   * Returns the stored output gain.
   *
   * @return The gain.
   */
  f32 output_gain() const override;

 private:
  struct Stream {
    std::move_only_function<void()> on_frame_played;
    u32 queued = 0;
  };

  void Tick();

  mutable std::mutex mutex_;
  std::map<u32, Stream> streams_;
  u32 next_stream_ = 1;
  StereoFold fold_;
  SurroundMix mix_;
  f32 gain_ = 1.0f;
  std::jthread clock_;
};

}  // namespace rex::apu
