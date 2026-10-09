/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <optional>

#include "gpu/xenos/pipeline/shader/interpreter.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register_file.h>
#include <rex/memory.h>

namespace rex::gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const xenos::RegisterFile& register_file, const runtime::Memory& memory)
      : register_file_(register_file),
        memory_(memory),
        shader_interpreter_(register_file, memory) {}

  /**
   * Returns the maximum Y in pixels covered by the vertices of the current draw.
   *
   * The vertex shader runs on the CPU for every index. For point lists, point sizes from the
   * vertex are clamped as signed integers in the hardware, so -NaN, -Infinity ... -0 go to the
   * minimum and +Infinity, +NaN go to the maximum. NaN positions are dropped since std::max
   * returns max_y, which starts as a normal value. The 16p8 range is -32768 to 32767+255/256 and
   * is stored as 24p8, so overflow up to -8388608 to 8388608+255/256 is safe. The window offset
   * is 15 bit signed, so adding it and the half pixel offset neither moves the 16p8 clamping
   * bounds into the 0...8192 screen space nor overflows 24p8.
   *
   * @param vertex_shader Vertex shader with analyzed ucode.
   * @return Exclusive maximum Y in pixels, 0 without indices, 8192 when not estimable.
   */
  u32 EstimateVertexMaxY(const Shader& vertex_shader);

  /**
   * Returns the maximum Y in pixels of the EDRAM extent involved in the draw.
   *
   * The scissor bounds the extent. Without clipping, vertices are processed on the CPU when
   * execute_unclipped_draw_vs_on_cpu is set. Unless
   * execute_unclipped_draw_vs_on_cpu_with_scissor is also set, that happens only when the scissor
   * reaches 8192 on both axes, the Direct3D 9 default, since 8192 alone may be a normal render
   * target height (80x8192 is within the EDRAM size). With clipping, the viewport bounds the
   * extent because hosts usually have a viewport scissor that cannot be disabled, although Xenos
   * has no implicit one. The viewport bottom is truncated as in draw_util::GetHostViewportInfo.
   * Taking max(0, bottom) drops NaN and negative values since max picks its first argument for
   * NaN, and min is done as float since max_y is well below 2^24.
   *
   * @param try_to_estimate_vertex_max_y True to allow processing vertices on the CPU.
   * @param vertex_shader Vertex shader with analyzed ucode.
   * @return Maximum Y in pixels.
   */
  u32 EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader);

 private:
  class PositionYExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Export(xenos::ucode::ExportRegister export_register, const f32* value,
                u32 value_mask) override;

    void Reset() {
      position_y_.reset();
      position_w_.reset();
      point_size_.reset();
      vertex_kill_.reset();
    }

    const std::optional<f32>& position_y() const { return position_y_; }
    const std::optional<f32>& position_w() const { return position_w_; }
    const std::optional<f32>& point_size() const { return point_size_; }
    const std::optional<u32>& vertex_kill() const { return vertex_kill_; }

   private:
    std::optional<f32> position_y_;
    std::optional<f32> position_w_;
    std::optional<f32> point_size_;
    std::optional<u32> vertex_kill_;
  };

  const xenos::RegisterFile& register_file_;
  const runtime::Memory& memory_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace rex::gpu
