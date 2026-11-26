/**
 * @file        tests/unit/graphics/shader_constant_overrides_test.cpp
 * @brief       Unit tests for per-shader float constant overrides
 *              (rex/graphics/util/shader_constant_overrides.h)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/util/shader_constant_overrides.h>

using rex::graphics::util::ShaderConstantOverrides;

namespace {

constexpr uint64_t kShader = 0x0F99810F072BF0FCull;

void Set(uint64_t map[4], uint32_t constant) {
  map[constant >> 6] |= uint64_t(1) << (constant & 63);
}

}  // namespace

TEST_CASE("ShaderConstantOverrides parsing", "[graphics][constant_overrides]") {
  ShaderConstantOverrides overrides;

  SECTION("an empty spec has no entries") {
    CHECK_FALSE(overrides.Update(""));  // unchanged from the initial empty text
    CHECK(overrides.empty());
  }

  SECTION("a valid entry is found by its ucode hash, whitespace ignored") {
    uint32_t malformed = 99;
    CHECK(overrides.Update(" 0F99810F072BF0FC : c47.z*rsx, c47.w*rsy, c77.x*0.75 ", &malformed));
    CHECK(malformed == 0);
    const auto* entry = overrides.Find(kShader);
    REQUIRE(entry != nullptr);
    CHECK(entry->ops.size() == 3);
    CHECK(overrides.Find(0x1234) == nullptr);
  }

  SECTION("the same text again reports no change") {
    CHECK(overrides.Update("12:c0.x*2"));
    CHECK_FALSE(overrides.Update("12:c0.x*2"));
  }

  SECTION("malformed entries are skipped and counted, valid ones kept") {
    uint32_t malformed = 0;
    overrides.Update(
        "12:c3.xw=1;junk;13:c300.x*2;14:c1.q*2;15:c1.x*;16:;:c1.x*2;17:c1.x*2,;18:c1.x/2",
        &malformed);
    CHECK(malformed == 8);
    CHECK(overrides.Find(0x12) != nullptr);
    CHECK(overrides.Find(0x13) == nullptr);
    CHECK(overrides.Find(0x17) == nullptr);
  }

  SECTION("several entries are separated by semicolons") {
    overrides.Update("1:c0.x*2;2:c1.y=3");
    CHECK(overrides.Find(1) != nullptr);
    CHECK(overrides.Find(2) != nullptr);
  }

  SECTION("clearing the spec removes the entries") {
    overrides.Update("1:c0.x*2");
    CHECK(overrides.Update(""));
    CHECK(overrides.empty());
  }
}

TEST_CASE("ShaderConstantOverrides::Apply", "[graphics][constant_overrides]") {
  ShaderConstantOverrides overrides;
  overrides.Update("0F99810F072BF0FC:c47.z*rsx,c47.w*rsy,c77.x*0.5;12:c3.xw=1");
  const auto& shadow = *overrides.Find(kShader);

  SECTION("rsx and rsy multiply by the draw resolution scale per axis") {
    float value[4] = {1.0f, 2.0f, 0.03125f, 0.03125f};
    ShaderConstantOverrides::Apply(shadow, 47, value, 3.0f, 2.0f);
    CHECK(value[0] == 1.0f);
    CHECK(value[1] == 2.0f);
    CHECK(value[2] == 0.09375f);
    CHECK(value[3] == 0.0625f);
  }

  SECTION("a number factor multiplies only the named components") {
    float value[4] = {0.002f, 9.0f, 9.0f, 9.0f};
    ShaderConstantOverrides::Apply(shadow, 77, value, 3.0f, 3.0f);
    CHECK(value[0] == 0.001f);
    CHECK(value[1] == 9.0f);
  }

  SECTION("constants without ops are untouched") {
    float value[4] = {5.0f, 5.0f, 5.0f, 5.0f};
    ShaderConstantOverrides::Apply(shadow, 48, value, 3.0f, 3.0f);
    CHECK(value[0] == 5.0f);
    CHECK(value[3] == 5.0f);
  }

  SECTION("= sets the value") {
    const auto& set = *overrides.Find(0x12);
    float value[4] = {5.0f, 5.0f, 5.0f, 5.0f};
    ShaderConstantOverrides::Apply(set, 3, value, 1.0f, 1.0f);
    CHECK(value[0] == 1.0f);
    CHECK(value[1] == 5.0f);
    CHECK(value[2] == 5.0f);
    CHECK(value[3] == 1.0f);
  }
}

TEST_CASE("ShaderConstantOverrides::ApplyToPacked", "[graphics][constant_overrides]") {
  ShaderConstantOverrides overrides;
  // Constants in different 64-bit words of the usage map, one of them unused by the shader.
  overrides.Update("1:c47.z*2,c77.x*0.5,c200.y=7,c5.x=9");
  const auto& entry = *overrides.Find(1);

  // The shader uses c2, c47, c64, c77 and c200 (not c5), packed in that order.
  uint64_t map[4] = {};
  const uint32_t used[] = {2, 47, 64, 77, 200};
  for (uint32_t constant : used) {
    Set(map, constant);
  }
  std::vector<float> packed(4 * 5);
  for (size_t i = 0; i < packed.size(); ++i) {
    packed[i] = 1.0f;
  }
  ShaderConstantOverrides::ApplyToPacked(entry, reinterpret_cast<uint8_t*>(packed.data()), map,
                                         3.0f, 3.0f);

  CHECK(packed[0 * 4 + 2] == 1.0f);  // c2: no op
  CHECK(packed[1 * 4 + 2] == 2.0f);  // c47.z
  CHECK(packed[1 * 4 + 0] == 1.0f);
  CHECK(packed[2 * 4 + 0] == 1.0f);  // c64: no op
  CHECK(packed[3 * 4 + 0] == 0.5f);  // c77.x
  CHECK(packed[4 * 4 + 1] == 7.0f);  // c200.y
  // c5 isn't in the shader's map, so nothing was written for it anywhere.
  for (size_t i = 0; i < packed.size(); ++i) {
    CHECK(packed[i] != 9.0f);
  }
}
