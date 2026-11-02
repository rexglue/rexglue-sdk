#include "apu/mix.h"

#include <algorithm>
#include <cmath>

namespace rex::apu {

void MixSurround(f32* output, const f32* planar, size_t count, const SurroundWeights& weights,
                 f32 gain) {
  const f32 level[6] = {gain,
                        gain,
                        weights.center * gain,
                        weights.lfe * gain,
                        weights.surround * gain,
                        weights.surround * gain};
  for (size_t channel = 0; channel < 6; ++channel) {
    const f32* in = planar + channel * count;
    f32* out = output + channel;
    const f32 weight = level[channel];
    for (size_t n = 0; n < count; ++n) {
      out[n * 6] = std::clamp(in[n] * weight, -1.0f, 1.0f);
    }
  }
}

void MixStereo(f32* output, const f32* planar, size_t count, const StereoWeights& weights,
               f32 gain) {
  const f32* fl = planar;
  const f32* fr = planar + count;
  const f32* fc = planar + 2 * count;
  const f32* lf = planar + 3 * count;
  const f32* bl = planar + 4 * count;
  const f32* br = planar + 5 * count;
  const f32 scale = weights.scale * gain;
  for (size_t n = 0; n < count; ++n) {
    const f32 mid = fc[n] * weights.center + lf[n] * weights.lfe;
    const f32 left = (fl[n] + mid + bl[n] * weights.surround) * scale;
    const f32 right = (fr[n] + mid + br[n] * weights.surround) * scale;
    output[n * 2] = std::clamp(left, -1.0f, 1.0f);
    output[n * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
  }
}

void RampGain(f32* planar, size_t count, f32 from, f32 to) {
  const f32 step = (to - from) / static_cast<f32>(count);
  for (size_t channel = 0; channel < 6; ++channel) {
    f32* samples = planar + channel * count;
    f32 level = from;
    for (size_t n = 0; n < count; ++n) {
      level += step;
      samples[n] *= level;
    }
  }
}

void StreamGuard::Apply(std::span<f32, rex::apu::kAudioFrameSamples> frame, f64 peak_limit) {
  f32 peak = 0.0f;
  bool finite = true;
  for (f32 sample : frame) {
    if (!std::isfinite(sample)) {
      finite = false;
      break;
    }
    peak = std::max(peak, std::fabs(sample));
  }
  if (!finite) {
    std::ranges::fill(frame, 0.0f);
    over_periods_ = kPeakPeriods;
    silenced_ = true;
    applied_ = 0.0f;
    return;
  }
  if (peak_limit > 0.0 && peak > static_cast<f32>(peak_limit)) {
    if (++over_periods_ >= kPeakPeriods) {
      silenced_ = true;
    }
  } else {
    over_periods_ = 0;
    silenced_ = false;
  }
  const f32 target = silenced_ ? 0.0f : 1.0f;
  if (applied_ != 1.0f || target != 1.0f) {
    const f32 step = (target - applied_) / static_cast<f32>(rex::apu::kAudioChannelSamples);
    for (size_t channel = 0; channel < rex::apu::kAudioFrameChannels; ++channel) {
      f32 level = applied_;
      for (size_t n = 0; n < rex::apu::kAudioChannelSamples; ++n) {
        frame[channel * rex::apu::kAudioChannelSamples + n] *= level;
        level += step;
      }
    }
  }
  applied_ = target;
}

}  // namespace rex::apu
