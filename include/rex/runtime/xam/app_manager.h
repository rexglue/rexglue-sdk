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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/memory.h>
#include <rex/runtime/xstatus.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {
class Memory;
}  // namespace rex::runtime

namespace rex {
namespace runtime {

/** Base class of an XAM app, which receives messages dispatched by app id. */
class REX_RUNTIME_API App {
 public:
  /**
   * Returns the id the app is registered under.
   *
   * @return The app id.
   */
  u32 GetAppId() const { return app_id_; }

  /**
   * Processes a message addressed to the app.
   *
   * @param message The message id.
   * @param buffer_ptr The guest address of the message buffer.
   * @param buffer_length The buffer length in bytes.
   * @return The result of the message.
   */
  virtual X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) = 0;

  virtual ~App() = default;

 protected:
  /**
   * Constructs the app.
   *
   * @param runtime The runtime that owns the app.
   * @param app_id The id the app is registered under.
   */
  App(Runtime& runtime, u32 app_id);

  Runtime& runtime_; /**< Runtime that owns the app. */
  /** Guest memory of the runtime, used to translate message buffer addresses. */
  runtime::Memory* memory_;
  u32 app_id_; /**< Id the app is registered under in the `AppManager`. */
};

/** Registry of XAM apps that routes messages by app id. */
class AppManager {
 public:
  /**
   * Adds an app, whose id must not be registered yet.
   *
   * @param app The app, owned by the manager afterwards.
   */
  REX_RUNTIME_API void RegisterApp(std::unique_ptr<App> app);

  /**
   * Sends a message to the app with the given id.
   *
   * @param app_id The id of the receiving app.
   * @param message The message id.
   * @param buffer_ptr The guest address of the message buffer.
   * @param buffer_length The buffer length in bytes.
   * @return X_E_NOTFOUND when no app has the id, else the result of the app.
   */
  REX_RUNTIME_API X_HRESULT DispatchMessageSync(u32 app_id, u32 message, u32 buffer_ptr,
                                                u32 buffer_length);
  /**
   * Sends an asynchronous message to the app with the given id. The app receives it through
   * DispatchMessageSync.
   *
   * @param app_id The id of the receiving app.
   * @param message The message id.
   * @param buffer_ptr The guest address of the message buffer.
   * @param buffer_length The buffer length in bytes.
   * @return X_E_NOTFOUND when no app has the id, else the result of the app.
   */
  REX_RUNTIME_API X_HRESULT DispatchMessageAsync(u32 app_id, u32 message, u32 buffer_ptr,
                                                 u32 buffer_length);

 private:
  std::vector<std::unique_ptr<App>> apps_;
  std::unordered_map<u32, App*> app_lookup_;
};

}  // namespace runtime
}  // namespace rex
