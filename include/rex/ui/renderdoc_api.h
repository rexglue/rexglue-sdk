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

#include <memory>

#include <rex/platform/dynlib.h>

#include <renderdoc_app.h>

namespace rex {
namespace ui {

class RenderDocAPI {
 public:
  static std::unique_ptr<RenderDocAPI> CreateIfConnected();

  RenderDocAPI(const RenderDocAPI&) = delete;
  RenderDocAPI& operator=(const RenderDocAPI&) = delete;

  ~RenderDocAPI();

  // Always present if this object exists.
  const RENDERDOC_API_1_0_0* api_1_0_0() const { return api_1_0_0_; }

 private:
  explicit RenderDocAPI() = default;

  rex::platform::DynamicLibrary library_;

  const RENDERDOC_API_1_0_0* api_1_0_0_ = nullptr;
};

}  // namespace ui
}  // namespace rex
