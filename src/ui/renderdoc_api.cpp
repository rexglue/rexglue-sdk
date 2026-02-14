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
#include <rex/ui/renderdoc_api.h>

namespace rex {
namespace ui {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
  std::unique_ptr<RenderDocAPI> renderdoc_api(new RenderDocAPI());

  pRENDERDOC_GetAPI get_api = nullptr;

  // Try to load the RenderDoc library. If RenderDoc is attached, the library
  // should already be loaded into the process and this will increment the
  // reference count. If not attached, the load will fail and we return nullptr.
  if (!renderdoc_api->library_.Load(platform::lib_names::kRenderDoc)) {
    return nullptr;
  }
  get_api = renderdoc_api->library_.GetSymbol<pRENDERDOC_GetAPI>("RENDERDOC_GetAPI");

  // get_api will be null if RenderDoc is not connected, or the API isn't
  // available on this platform, or there was an error.
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, (void**)&renderdoc_api->api_1_0_0_) ||
      !renderdoc_api->api_1_0_0_) {
    return nullptr;
  }

  REXLOG_INFO("RenderDoc API initialized");

  return renderdoc_api;
}

RenderDocAPI::~RenderDocAPI() {
  library_.Close();
}

}  // namespace ui
}  // namespace rex
