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

#include <rex/logging.h>
#include "gpu/xenos/host/renderdoc_api.h"

namespace rex {
namespace gpu {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
#if REX_PLATFORM_MAC
  return nullptr;
#else
  auto renderdoc_api = std::make_unique<RenderDocAPI>(Key{});

  pRENDERDOC_GetAPI get_api = nullptr;

  if (!renderdoc_api->library_.Load(platform::lib_names::kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  void* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, &api) || !api) {
    return nullptr;
  }
  renderdoc_api->api_1_0_0_ = static_cast<const RENDERDOC_API_1_0_0*>(api);

  REXLOG_INFO(ui, "RenderDoc API initialized");

  return renderdoc_api;
#endif
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace gpu
}  // namespace rex
