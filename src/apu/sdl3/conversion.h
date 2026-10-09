/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

#include <rex/assert.h>
#include <rex/memory/utils.h>
#include <rex/apu/backend.h>
#include <rex/platform.h>
#include <rex/types.h>

#include <simde/x86/ssse3.h>

namespace rex::apu::sdl3 {

/**
 * Converts 6 sequential big endian channels to interleaved little endian with the surround mix
 * and gain applied. The gain pass walks 4 floats at a time and the 6 channel interleave repeats
 * every 12 floats, so 3 weight vectors cover every alignment. An even sample count makes the
 * float count a multiple of 12.
 *
 * @param output Interleaved output, 6 floats per sample.
 * @param input Sequential input, 6 channels of ch_sample_count samples.
 * @param ch_sample_count Samples per channel, even.
 * @param mix Weights for the center, surround and LFE channels.
 * @param gain Output gain, results are clamped to the range -1 to 1.
 */
inline void sequential_6_BE_to_interleaved_6_LE(f32* output, const f32* input,
                                                size_t ch_sample_count, const SurroundMix& mix,
                                                f32 gain) {
  REX_ASSERT(ch_sample_count % 2 == 0);

  const u32* in = reinterpret_cast<const u32*>(input);
  u32* out = reinterpret_cast<u32*>(output);
  const simde__m128i byte_swap_shuffle =
      simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  for (size_t sample = 0; sample < ch_sample_count; sample++) {
    simde__m128i sample0 =
        simde_mm_set_epi32(in[3 * ch_sample_count + sample], in[2 * ch_sample_count + sample],
                           in[1 * ch_sample_count + sample], in[0 * ch_sample_count + sample]);
    u32 sample1 = in[4 * ch_sample_count + sample];
    u32 sample2 = in[5 * ch_sample_count + sample];
    sample0 = simde_mm_shuffle_epi8(sample0, byte_swap_shuffle);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(&out[sample * 6]), sample0);
    sample1 = std::byteswap(sample1);
    out[sample * 6 + 4] = sample1;
    sample2 = std::byteswap(sample2);
    out[sample * 6 + 5] = sample2;
  }

  const f32 c = mix.center * gain;
  const f32 s = mix.surround * gain;
  const f32 l = mix.lfe * gain;
  const simde__m128 w[3] = {simde_mm_setr_ps(gain, gain, c, l), simde_mm_setr_ps(s, s, gain, gain),
                            simde_mm_setr_ps(c, l, s, s)};
  const simde__m128 lo = simde_mm_set1_ps(-1.0f);
  const simde__m128 hi = simde_mm_set1_ps(1.0f);
  const size_t count = ch_sample_count * 6;
  for (size_t i = 0, phase = 0; i < count; i += 4, phase = (phase + 1) % 3) {
    const simde__m128 v = simde_mm_mul_ps(simde_mm_loadu_ps(&output[i]), w[phase]);
    simde_mm_storeu_ps(&output[i], simde_mm_min_ps(simde_mm_max_ps(v, lo), hi));
  }
}

inline void sequential_6_BE_to_interleaved_2_LE(f32* output, const f32* input,
                                                size_t ch_sample_count, const StereoFold& fold,
                                                f32 gain) {
  REX_ASSERT(ch_sample_count % 4 == 0);

  const simde__m128i byte_swap_shuffle =
      simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
  const simde__m128 center = simde_mm_set1_ps(fold.center);
  const simde__m128 lfe = simde_mm_set1_ps(fold.lfe);
  const simde__m128 surround = simde_mm_set1_ps(fold.surround);
  const simde__m128 scale = simde_mm_set1_ps(fold.scale * gain);
  const simde__m128 lo = simde_mm_set1_ps(-1.0f);
  const simde__m128 hi = simde_mm_set1_ps(1.0f);

  for (size_t sample = 0; sample < ch_sample_count; sample += 4) {
    simde__m128 fl = simde_mm_loadu_ps(&input[0 * ch_sample_count + sample]);
    simde__m128 fr = simde_mm_loadu_ps(&input[1 * ch_sample_count + sample]);
    simde__m128 fc = simde_mm_loadu_ps(&input[2 * ch_sample_count + sample]);
    simde__m128 lf = simde_mm_loadu_ps(&input[3 * ch_sample_count + sample]);
    simde__m128 bl = simde_mm_loadu_ps(&input[4 * ch_sample_count + sample]);
    simde__m128 br = simde_mm_loadu_ps(&input[5 * ch_sample_count + sample]);
    fl = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(fl), byte_swap_shuffle));
    fr = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(fr), byte_swap_shuffle));
    fc = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(fc), byte_swap_shuffle));
    lf = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(lf), byte_swap_shuffle));
    bl = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(bl), byte_swap_shuffle));
    br = simde_mm_castsi128_ps(simde_mm_shuffle_epi8(simde_mm_castps_si128(br), byte_swap_shuffle));

    const simde__m128 mid = simde_mm_add_ps(simde_mm_mul_ps(fc, center), simde_mm_mul_ps(lf, lfe));
    simde__m128 left = simde_mm_mul_ps(
        simde_mm_add_ps(simde_mm_add_ps(fl, mid), simde_mm_mul_ps(bl, surround)), scale);
    simde__m128 right = simde_mm_mul_ps(
        simde_mm_add_ps(simde_mm_add_ps(fr, mid), simde_mm_mul_ps(br, surround)), scale);
    left = simde_mm_min_ps(simde_mm_max_ps(left, lo), hi);
    right = simde_mm_min_ps(simde_mm_max_ps(right, lo), hi);

    simde_mm_storeu_ps(&output[sample * 2], simde_mm_unpacklo_ps(left, right));
    simde_mm_storeu_ps(&output[(sample + 2) * 2], simde_mm_unpackhi_ps(left, right));
  }
}

}  // namespace rex::apu::sdl3
