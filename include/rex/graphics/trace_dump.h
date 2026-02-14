#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/trace_player.h>
#include <rex/graphics/trace_protocol.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/runtime.h>

namespace rex::graphics {

struct SamplerInfo;
struct TextureInfo;

class TraceDump {
 public:
  virtual ~TraceDump();

  int Main(const std::vector<std::string>& args);

 protected:
  TraceDump();

  virtual std::unique_ptr<GraphicsSystem> CreateGraphicsSystem() = 0;

  virtual void BeginHostCapture() = 0;
  virtual void EndHostCapture() = 0;

  std::unique_ptr<Runtime> emulator_;
  GraphicsSystem* graphics_system_ = nullptr;
  std::unique_ptr<TracePlayer> player_;

 private:
  bool Setup();
  bool Load(const std::filesystem::path& trace_file_path);
  int Run();

  std::filesystem::path trace_file_path_;
  std::filesystem::path base_output_path_;
};

}  // namespace rex::graphics
