/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/title_runtime.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/title_lifecycle.h>

namespace rex::xam {

XAMApplet::XAMApplet(const XAMKernelInterfaces& title, u32 app_id)
    : title_(title), membase_(title.membase), app_id_(app_id) {}

void AppManager::RegisterApp(std::unique_ptr<XAMApplet> app) {
  REX_ASSERT(app_lookup_.count(app->GetAppID()) == 0);
  app_lookup_.insert({app->GetAppID(), app.get()});
  apps_.push_back(std::move(app));
}

rex::kernel::X_HRESULT AppManager::DispatchMessageSync(u32 app_id, u32 message, u32 buffer_ptr,
                                                       u32 buffer_length) {
  XAMApplet* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return rex::kernel::X_E_NOTFOUND;
    }
    app = it->second;
  }
  return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

rex::kernel::X_HRESULT AppManager::DispatchMessageAsync(u32 app_id, u32 message, u32 buffer_ptr,
                                                        u32 buffer_length) {
  XAMApplet* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return rex::kernel::X_E_NOTFOUND;
    }
    app = it->second;
  }
  return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

}  // namespace rex::xam
