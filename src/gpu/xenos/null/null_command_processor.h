/**
 * @file        gpu/xenos/null/null_command_processor.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <unordered_map>

#include "gpu/xenos/command_processor.h"
#include "gpu/xenos/pipeline/shader/shader.h"

namespace rex::gpu::null {

/** Runs PM4, fences, memory and register writes and interrupts. Skips draws, copies and swaps. */
class NullCommandProcessor final : public CommandProcessor {
 public:
  /**
   * @param graphics_system The graphics system that owns the processor.
   * @param runtime The runtime the worker thread belongs to.
   */
  NullCommandProcessor(GraphicsSystem* graphics_system, Runtime& runtime);
  ~NullCommandProcessor() override;

  void IssueSwap(u32 frontbuffer_ptr, u32 frontbuffer_width, u32 frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  Shader* LoadShader(xenos::ShaderType shader_type, u32 guest_address, const u32* host_address,
                     u32 dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, u32 index_count, IndexBuffer* index_buffer_info,
                 bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  std::unordered_map<u64, std::unique_ptr<Shader>> shaders_;
};

}  // namespace rex::gpu::null
