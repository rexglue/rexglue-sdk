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

#pragma once

#include <memory>

#include <rex/platform/dynlib.h>
#include <rex/types.h>

#if REX_PLATFORM_MAC
struct RENDERDOC_API_1_0_0;
#else
#include <renderdoc_app.h>
#endif

namespace rex {
namespace gpu {

class RenderDocAPI : public rex::NonCopyable {
  struct Key {};

 public:
  explicit RenderDocAPI(Key) {}

  /**
   * Creates the API object when RenderDoc is attached to the process.
   *
   * Always returns nullptr on macOS, where the vendored header exposes no API.
   *
   * @return The API object, or nullptr when RenderDoc is not connected.
   */
  static std::unique_ptr<RenderDocAPI> CreateIfConnected();

  ~RenderDocAPI();

  /** Always present when this object exists. */
  const RENDERDOC_API_1_0_0* GetAPI100() const { return api_1_0_0_; }

 private:
  rex::platform::DynamicLibrary library_;

  const RENDERDOC_API_1_0_0* api_1_0_0_ = nullptr;
};

}  // namespace gpu
}  // namespace rex
