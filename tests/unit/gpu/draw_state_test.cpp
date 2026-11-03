/**
 * @file        tests/unit/gpu/draw_state_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>

#include <rex/xenos/register.h>
#include <rex/xenos/state.h>

#include "gpu/xenos/draw_state.h"

namespace {

using rex::xenos::CompareFunction;
using rex::xenos::EDRAMMode;
using rex::xenos::RegisterFile;
namespace reg = rex::xenos::reg;

template <typename T>
void Write(RegisterFile& regs, const T& value) {
  regs[T::kRegisterIndex] = value.value;
}

std::unique_ptr<RegisterFile> MakeDraw(CompareFunction zfunc, bool z_enable, bool stencil_enable) {
  auto regs = std::make_unique<RegisterFile>();
  reg::RB_MODECONTROL mode{};
  mode.edram_mode = EDRAMMode::kColorDepth;
  Write(*regs, mode);
  reg::RB_SURFACE_INFO surface{};
  surface.surface_pitch = 1280;
  Write(*regs, surface);
  reg::RB_DEPTHCONTROL depth{};
  depth.z_enable = z_enable;
  depth.zfunc = zfunc;
  depth.stencil_enable = stencil_enable;
  Write(*regs, depth);
  return regs;
}

}  // namespace

REX_TEST_CASE("A depth test that never passes ends rasterization without a pixel shader memexport",
              "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kNever, true, false);
  CHECK(rex::gpu::draw_state::IsRasterizationPotentiallyDone(*regs, true));
  CHECK_FALSE(rex::gpu::draw_state::IsRasterizationDone(*regs, true, false));
}

REX_TEST_CASE("A stencil test keeps rasterization after a depth test that never passes",
              "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kNever, true, true);
  CHECK(rex::gpu::draw_state::IsRasterizationDone(*regs, true, false));
}

REX_TEST_CASE("A pixel shader memexport keeps rasterization after a depth test that never passes",
              "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kNever, true, false);
  CHECK(rex::gpu::draw_state::IsRasterizationDone(*regs, true, true));
}

REX_TEST_CASE("Other depth functions keep rasterization", "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kLess, true, false);
  CHECK(rex::gpu::draw_state::IsRasterizationDone(*regs, true, false));
}

REX_TEST_CASE("The depth function is ignored without the depth test", "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kNever, false, false);
  CHECK(rex::gpu::draw_state::IsRasterizationDone(*regs, true, false));
}

REX_TEST_CASE("The reasons to skip rasterization apply", "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kLess, true, false);
  reg::RB_SURFACE_INFO surface{};
  Write(*regs, surface);
  CHECK_FALSE(rex::gpu::draw_state::IsRasterizationDone(*regs, true, true));

  regs = MakeDraw(CompareFunction::kLess, true, false);
  reg::SQ_PROGRAM_CNTL program{};
  program.vs_export_mode = rex::xenos::VertexShaderExportMode::kMultipass;
  Write(*regs, program);
  CHECK_FALSE(rex::gpu::draw_state::IsRasterizationDone(*regs, true, true));

  regs = MakeDraw(CompareFunction::kLess, true, false);
  reg::PA_SU_SC_MODE_CNTL culling{};
  culling.cull_front = 1;
  culling.cull_back = 1;
  Write(*regs, culling);
  CHECK_FALSE(rex::gpu::draw_state::IsRasterizationDone(*regs, true, true));
  CHECK(rex::gpu::draw_state::IsRasterizationDone(*regs, false, true));
}

REX_TEST_CASE("The normalized depth control of a never passing test is unchanged",
              "[gpu][draw_state]") {
  auto regs = MakeDraw(CompareFunction::kNever, true, false);
  const reg::RB_DEPTHCONTROL normalized = rex::gpu::draw_state::GetNormalizedDepthControl(*regs);
  CHECK(normalized.z_enable == 1);
  CHECK(normalized.zfunc == CompareFunction::kNever);
}
