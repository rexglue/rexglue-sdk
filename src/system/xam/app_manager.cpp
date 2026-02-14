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

#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace rex {
namespace system {
namespace xam {

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state), memory_(kernel_state->memory()), app_id_(app_id) {}

void AppManager::RegisterApps(KernelState* kernel_state, AppManager* manager) {
  manager->RegisterApp(std::make_unique<kernel::xam::apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<kernel::xam::apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<kernel::xam::apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<kernel::xam::apps::XamApp>(kernel_state));
}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  assert_zero(app_lookup_.count(app->app_id()));
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message, uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  return it->second->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message, uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  return it->second->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

}  // namespace xam
}  // namespace system
}  // namespace rex
