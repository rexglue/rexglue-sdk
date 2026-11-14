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
#include "gpu/xenos/xenos_settings.h"
#include <rex/xenos/register.h>
#include <rex/memory.h>
#include <rex/guest.h>

namespace rex::gpu {

class DrawExtentEstimator {
 public:
  DrawExtentEstimator(const xenos::RegisterFile& register_file, const XenosSettings& settings,
                      IPhysicalMemory& memory)
      : register_file_(register_file),
        settings_(settings),
        physical_memory_(memory),
        shader_interpreter_(register_file, memory) {}

  /**
   * Returns the maximum Y in pixels covered by the vertices of the current draw. The bound is
   * exclusive, 0 without indices and 8192 when not estimable.
   *
   * @param vertex_shader Vertex shader with analyzed ucode.
   */
  u32 EstimateVertexMaxY(const Shader& vertex_shader);

  /**
   * Returns the maximum Y in pixels of the EDRAM extent involved in the draw.
   *
   * @param try_to_estimate_vertex_max_y True to allow processing vertices on the CPU.
   * @param vertex_shader Vertex shader with analyzed ucode.
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

    const std::optional<f32>& GetPositionY() const { return position_y_; }
    const std::optional<f32>& GetPositionW() const { return position_w_; }
    const std::optional<f32>& GetPointSize() const { return point_size_; }
    const std::optional<u32>& GetVertexKill() const { return vertex_kill_; }

   private:
    std::optional<f32> position_y_;
    std::optional<f32> position_w_;
    std::optional<f32> point_size_;
    std::optional<u32> vertex_kill_;
  };

  const xenos::RegisterFile& register_file_;
  const XenosSettings& settings_;
  IPhysicalMemory& physical_memory_;

  ShaderInterpreter shader_interpreter_;
};

}  // namespace rex::gpu
