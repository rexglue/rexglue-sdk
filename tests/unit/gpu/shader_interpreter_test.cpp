/**
 * @file        tests/unit/gpu/shader_interpreter_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <bit>
#include <memory>
#include <utility>

#include <rex/types.h>
#include <rex/xenos/register.h>
#include <rex/xenos/ucode.h>

#include "gpu/xenos/pipeline/shader/interpreter.h"
#include "fake_title_context.h"
#include "test_memory.h"

namespace {

using rex::f32;
using rex::u32;

constexpr u32 kExecEnd = std::to_underlying(rex::xenos::ucode::ControlFlowOpcode::kExecEnd);
constexpr u32 kMulsc0 = std::to_underlying(rex::xenos::ucode::AluScalarOpcode::kMulsc0);

std::array<u32, 6> MulConstTempProgram(bool abs_constants) {
  std::array<u32, 6> ucode{};
  ucode[0] = 1 | (1 << 12);
  ucode[1] = kExecEnd << 12;
  ucode[3] = (static_cast<u32>(abs_constants) << 7) | (1 << 8) | (0b0001 << 20) | (kMulsc0 << 26);
  return ucode;
}

f32 RunMulConstTemp(bool abs_constants) {
  auto registers = std::make_unique<rex::xenos::RegisterFile>();
  (*registers)[rex::xenos::Register::SHADER_CONSTANT_000_W] = std::bit_cast<u32>(-2.0f);
  rex::test::FakeTitleContext title(rex::test::GetTestMemory());
  rex::gpu::ShaderInterpreter interpreter(*registers, title);
  interpreter.GetTempRegisters()[0] = 3.0f;
  const auto ucode = MulConstTempProgram(abs_constants);
  interpreter.SetShader(rex::xenos::ShaderType::kVertex, ucode.data());
  interpreter.Execute();
  return interpreter.GetTempRegisters()[4];
}

}  // namespace

REX_TEST_CASE("A scalar constant and temp operation takes the absolute value of both operands",
              "[gpu][shader][interpreter]") {
  CHECK(RunMulConstTemp(true) == 6.0f);
}

REX_TEST_CASE(
    "A scalar constant and temp operation keeps the operand signs without the absolute flag",
    "[gpu][shader][interpreter]") {
  CHECK(RunMulConstTemp(false) == -6.0f);
}

REX_TEST_CASE("Transcendental results are rounded to 21 mantissa bits, halfway away from zero",
              "[gpu][shader][interpreter]") {
  using rex::gpu::ShaderInterpreter;
  constexpr u32 kBits = rex::gpu::kTranscendentalMantissaBits;
  const auto reduce = [](u32 bits) {
    return std::bit_cast<u32>(
        ShaderInterpreter::ReduceFloatPrecision(std::bit_cast<f32>(bits), kBits));
  };
  // 1 / 3 is 0x3EAAAAAB: the two discarded bits are 0b11, so it rounds up, away from zero.
  CHECK(reduce(0x3EAAAAABu) == 0x3EAAAAACu);
  CHECK(reduce(0xBEAAAAABu) == 0xBEAAAAACu);
  // Halfway (0b10) rounds up, below it (0b01) rounds down.
  CHECK(reduce(0x3F800002u) == 0x3F800004u);
  CHECK(reduce(0x3F800001u) == 0x3F800000u);
  // A value that fits in 21 bits is unchanged.
  CHECK(reduce(0x3F000000u) == 0x3F000000u);
  // The largest finite value keeps its truncated bits instead of rounding to infinity.
  CHECK(reduce(0x7F7FFFFFu) == 0x7F7FFFFCu);
  // Infinity, NaN and signed zero are unchanged.
  CHECK(reduce(0x7F800000u) == 0x7F800000u);
  CHECK(reduce(0xFF800000u) == 0xFF800000u);
  CHECK(reduce(0x7FC00001u) == 0x7FC00001u);
  CHECK(reduce(0x80000000u) == 0x80000000u);
}
