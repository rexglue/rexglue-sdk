#pragma once
// Xenia's magic-division helper: upstream dropped it, the ported viewport maths still uses it.
#include <cstdint>
namespace rex {
namespace divisors {
union IDivExtraInfo {
  uint32_t value_;
  struct {
    uint32_t shift_ : 31;
    uint32_t add_ : 1;
  } info;
};
// Returns the magic multiplier and writes the shift/add extra data.
static constexpr uint32_t PregenerateUint32Div(uint32_t _denom, uint32_t& out_extra) {
  IDivExtraInfo extra{};

  uint32_t d = _denom;
  int p = 0;
  uint32_t nc = 0, delta = 0, q1 = 0, r1 = 0, q2 = 0, r2 = 0;
  struct {
    unsigned M;
    int a;
    int s;
  } magu{};
  magu.a = 0;
  nc = -1 - ((uint32_t)-(int32_t)d) % d;
  p = 31;
  q1 = 0x80000000 / nc;
  r1 = 0x80000000 - q1 * nc;
  q2 = 0x7FFFFFFF / d;
  r2 = 0x7FFFFFFF - q2 * d;
  do {
    p += 1;
    if (r1 >= nc - r1) {
      q1 = 2 * q1 + 1;
      r1 = 2 * r1 - nc;
    } else {
      q1 = 2 * q1;
      r1 = 2 * r1;
    }
    if (r2 + 1 >= d - r2) {
      if (q2 >= 0x7FFFFFFF) {
        magu.a = 1;
      }
      q2 = 2 * q2 + 1;
      r2 = 2 * r2 + 1 - d;

    } else {
      if (q2 >= 0x80000000U) {
        magu.a = 1;
      }
      q2 = 2 * q2;
      r2 = 2 * r2 + 1;
    }
    delta = d - 1 - r2;
  } while (p < 64 && (q1 < delta || r1 == 0));

  extra.info.add_ = magu.a;
  extra.info.shift_ = p - 32;
  out_extra = extra.value_;
  return static_cast<uint64_t>(q2 + 1);
}

static constexpr uint32_t ApplyUint32Div(uint32_t num, uint32_t mul, uint32_t extradata) {
  IDivExtraInfo extra{};

  extra.value_ = extradata;

  uint32_t result = static_cast<uint32_t>((static_cast<uint64_t>(num) * static_cast<uint64_t>(mul)) >> 32);
  if (extra.info.add_) {
    uint32_t addend = result + num;
    addend = ((addend < result ? 0x80000000 : 0) | addend);
    result = addend;
  }
  return result >> extra.info.shift_;
}

static constexpr uint32_t ApplyUint32UMod(uint32_t num, uint32_t mul, uint32_t extradata, uint32_t original) {
  uint32_t dived = ApplyUint32Div(num, mul, extradata);
  unsigned result = num - (dived * original);

  return result;
}

struct MagicDiv {
  uint32_t multiplier_;
  uint32_t extradata_;
  constexpr MagicDiv() : multiplier_(0), extradata_(0) {}
  constexpr MagicDiv(uint32_t original) : MagicDiv() { multiplier_ = PregenerateUint32Div(original, extradata_); }

  constexpr uint32_t GetRightShift() const {
    IDivExtraInfo extra{};

    extra.value_ = extradata_;
    return extra.info.shift_;
  }

  constexpr bool AddFlag() const {
    IDivExtraInfo extra{};

    extra.value_ = extradata_;
    return extra.info.shift_;
  }

  constexpr uint32_t GetMultiplier() const { return multiplier_; }
  constexpr uint32_t Apply(uint32_t numerator) const {
    return ApplyUint32Div(numerator, multiplier_, extradata_);
  }
};
}  // namespace divisors
}  // namespace rex
