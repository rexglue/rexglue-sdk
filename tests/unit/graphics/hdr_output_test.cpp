/**
 * @file        tests/unit/graphics/hdr_output_test.cpp
 * @brief       Unit tests for the HDR output path's color helpers
 *              (rex/graphics/util/hdr_output.h)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cmath>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/util/hdr_output.h>
#include <rex/graphics/xenos.h>

using rex::graphics::util::RepackColorClearAsFloat7e3;
using rex::graphics::util::UnormToFloat7e3;
using rex::graphics::xenos::Float7e3To32;

namespace {

// Half the distance between neighbouring 7e3 values around `value`, in [0, 1].
float HalfStep7e3(float value) {
  if (value < 0.25f) {
    return 0.5f / 512.0f;
  }
  int exponent;
  std::frexp(value, &exponent);
  return std::ldexp(1.0f, exponent - 1) / 256.0f;
}

}  // namespace

TEST_CASE("UnormToFloat7e3", "[graphics][hdr_output]") {
  SECTION("every 7e3 value in [0, 1] round-trips exactly") {
    for (uint32_t bits = 0; bits <= (3u << 7); ++bits) {
      CHECK(UnormToFloat7e3(Float7e3To32(bits)) == bits);
    }
  }

  SECTION("8-bit and 10-bit unorm values land on the nearest 7e3 value") {
    for (uint32_t i = 0; i <= 1023; ++i) {
      float value = float(i) / 1023.0f;
      float converted = Float7e3To32(UnormToFloat7e3(value));
      CHECK(std::fabs(converted - value) <= HalfStep7e3(value) * 1.0001f);
    }
    for (uint32_t i = 0; i <= 255; ++i) {
      float value = float(i) / 255.0f;
      float converted = Float7e3To32(UnormToFloat7e3(value));
      CHECK(std::fabs(converted - value) <= HalfStep7e3(value) * 1.0001f);
    }
  }

  SECTION("out of range values are clamped") {
    CHECK(UnormToFloat7e3(-1.0f) == 0);
    CHECK(UnormToFloat7e3(NAN) == 0);
    CHECK(UnormToFloat7e3(2.0f) == (3u << 7));
  }
}

TEST_CASE("RepackColorClearAsFloat7e3", "[graphics][hdr_output]") {
  SECTION("8888: white, black and alpha") {
    CHECK(RepackColorClearAsFloat7e3(0xFFFFFFFF, true) ==
          ((3u << 30) | (0x180u << 20) | (0x180u << 10) | 0x180u));
    CHECK(RepackColorClearAsFloat7e3(0x00000000, true) == 0);
    CHECK((RepackColorClearAsFloat7e3(0x80000000, true) >> 30) == 2);  // 128/255*3 rounds to 2
  }

  SECTION("2:10:10:10: channels and alpha carry over") {
    uint32_t packed = (1u << 30) | (1023u << 20) | (0u << 10) | 1023u;
    uint32_t result = RepackColorClearAsFloat7e3(packed, false);
    CHECK((result >> 30) == 1);
    CHECK(((result >> 20) & 0x3FF) == 0x180);
    CHECK(((result >> 10) & 0x3FF) == 0);
    CHECK((result & 0x3FF) == 0x180);
  }

  SECTION("mid grey matches the decoded value") {
    uint32_t result = RepackColorClearAsFloat7e3(0x00808080, true);
    float value = Float7e3To32(result & 0x3FF);
    CHECK(std::fabs(value - 128.0f / 255.0f) <= HalfStep7e3(128.0f / 255.0f) * 1.0001f);
  }
}
