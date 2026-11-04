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

#include <rex/logging/log.h>
#include <rex/platform.h>
#include "gpu/xenos/host/library_name.h"
#include "gpu/xenos/host/renderdoc_api.h"

namespace rex {
namespace gpu {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  return nullptr;
#else
  auto renderdoc_api = std::make_unique<RenderDocAPI>(Key{});

  pRENDERDOC_GetAPI get_api = nullptr;

  if (!renderdoc_api->library_.Attach(kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  void* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, &api) || !api) {
    return nullptr;
  }
  renderdoc_api->api_1_0_0_ = static_cast<const RENDERDOC_API_1_0_0*>(api);

  REXUI_INFO("RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace gpu
}  // namespace rex
