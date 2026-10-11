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

#include <rex/runtime.h>
#include <rex/runtime/xam/app_manager.h>

namespace rex {
namespace runtime {

App::App(Runtime& runtime, u32 app_id)
    : runtime_(runtime), memory_(runtime.memory()), app_id_(app_id) {}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  REX_ASSERT(app_lookup_.count(app->app_id()) == 0);
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(u32 app_id, u32 message, u32 buffer_ptr,
                                          u32 buffer_length) {
  App* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return X_E_NOTFOUND;
    }
    app = it->second;
  }
  return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

X_HRESULT AppManager::DispatchMessageAsync(u32 app_id, u32 message, u32 buffer_ptr,
                                           u32 buffer_length) {
  App* app;
  {
    auto it = app_lookup_.find(app_id);
    if (it == app_lookup_.end()) {
      return X_E_NOTFOUND;
    }
    app = it->second;
  }
  return app->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

}  // namespace runtime
}  // namespace rex
