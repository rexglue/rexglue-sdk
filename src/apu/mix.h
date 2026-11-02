/**
 * @file        apu/mix.h
 * @brief       Channel folding, gain ramps and the stream guard of the mix
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <span>

#include <rex/apu/audio_driver.h>
#include <rex/types.h>

namespace rex::apu {

/** Weights for a device with six channels. A weight of 1.0 passes the channel through. */
struct SurroundWeights {
  f32 center = 1.0f;
  f32 surround = 1.0f;
  f32 lfe = 1.0f;
};

/**
 * Weights for folding to a stereo device. The front channels have an implicit weight of 1.
 * The defaults follow ITU-R BS.775 without the LFE.
 */
struct StereoWeights {
  f32 center = 0.70710678f;
  f32 surround = 0.70710678f;
  f32 lfe = 0.0f;
  f32 scale = 0.58578644f;
};

/** Periods a stream may stay over apu.peak_limit before the guard silences it. */
inline constexpr u32 kPeakPeriods = 2;

/**
 * Interleaves six sequential channels with the surround weights and gain applied. Results are
 * clamped to the range -1 to 1.
 *
 * @param output Interleaved output, six floats per sample.
 * @param planar Sequential input, six channels of count samples.
 * @param count Samples per channel.
 * @param weights The weights of the center, surround and LFE channels.
 * @param gain The output gain.
 */
void MixSurround(f32* output, const f32* planar, size_t count, const SurroundWeights& weights,
                 f32 gain);

/**
 * Folds six sequential channels to interleaved stereo with the gain applied. Results are clamped
 * to the range -1 to 1.
 *
 * @param output Interleaved output, two floats per sample.
 * @param planar Sequential input, six channels of count samples.
 * @param count Samples per channel.
 * @param weights The fold weights.
 * @param gain The output gain.
 */
void MixStereo(f32* output, const f32* planar, size_t count, const StereoWeights& weights,
               f32 gain);

/**
 * Multiplies six sequential channels by a gain that moves in equal steps to the new level,
 * reached on the last sample.
 *
 * @param count Samples per channel.
 * @param from The level before the first sample.
 * @param to The level on the last sample.
 */
void RampGain(f32* planar, size_t count, f32 from, f32 to);

/**
 * Silences a stream whose samples are not finite or stay over the peak limit for kPeakPeriods
 * periods. It ramps back once a frame comes in under the limit.
 */
class StreamGuard {
 public:
  /**
   * Applies the guard to one frame in place.
   *
   * @param frame Six sequential channels of kAudioFrameSamples samples in total.
   * @param peak_limit The limit, 0 for none.
   */
  void Apply(std::span<f32, rex::apu::kAudioFrameSamples> frame, f64 peak_limit);

 private:
  f32 applied_ = 1.0f;
  u32 over_periods_ = 0;
  bool silenced_ = false;
};

}  // namespace rex::apu
