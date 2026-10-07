/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/macros.h>
#include <rex/memory.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {
class Memory;
}  // namespace rex::runtime

namespace rex {
namespace runtime {
namespace xam {

class REX_RUNTIME_API App {
 public:
  uint32_t app_id() const { return app_id_; }

  virtual X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                        uint32_t buffer_length) = 0;

  virtual ~App() = default;

 protected:
  App(Runtime& runtime, uint32_t app_id);

  Runtime& runtime_;
  runtime::Memory* memory_;
  uint32_t app_id_;
};

class AppManager {
 public:
  REX_RUNTIME_API void RegisterApp(std::unique_ptr<App> app);

  REX_RUNTIME_API X_HRESULT DispatchMessageSync(uint32_t app_id, uint32_t message,
                                                uint32_t buffer_ptr, uint32_t buffer_length);
  REX_RUNTIME_API X_HRESULT DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                                 uint32_t buffer_ptr, uint32_t buffer_length);

 private:
  std::vector<std::unique_ptr<App>> apps_;
  std::unordered_map<uint32_t, App*> app_lookup_;
};

}  // namespace xam
}  // namespace runtime
}  // namespace rex
