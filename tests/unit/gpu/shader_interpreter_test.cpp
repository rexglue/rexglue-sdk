/**
 * @file        tests/unit/gpu/shader_interpreter_test.cpp
 * @brief       The shader interpreter runs scalar constant and temp operations as the translators
 * do
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
#include <rex/xenos/register_file.h>
#include <rex/xenos/ucode.h>

#include "gpu/xenos/pipeline/shader/interpreter.h"
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
  rex::gpu::ShaderInterpreter interpreter(*registers, rex::test::GetTestMemory());
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
