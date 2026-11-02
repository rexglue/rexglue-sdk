/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        xam/app_manager.h
 * @brief       XAM app base class and the registry that routes messages by app ID
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 */

#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>
#include <format>
#include <string>
#include <filesystem>
#include <optional>
#include <string_view>
#include <functional>
#include <span>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/xam/title_lifecycle.h>
#include <rex/kernel/types/status.h>
#include <rex/memory/stream.h>
#include <rex/kernel/types/content.h>
#include <rex/result.h>
#include <rex/filesystem/xdbf.h>
#include <rex/utility.h>
#include <rex/title.h>

namespace rex::xam {

/** Base class of an XAM app, which receives messages dispatched by app ID. */
class REX_XAM_API XAMApplet {
 public:
  u32 GetAppID() const { return app_id_; }

  /**
   * Processes a message addressed to the app.
   *
   * @param buffer_ptr The guest address of the message buffer.
   */
  virtual rex::kernel::X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr,
                                                     u32 buffer_length) = 0;

  virtual ~XAMApplet() = default;

 protected:
  /**
   * @param title The kernel interfaces of the title the app serves.
   * @param app_id The ID the app is registered under.
   */
  XAMApplet(const XAMKernelInterfaces& title, u32 app_id);

  XAMKernelInterfaces title_;
  /** Translates the message buffer addresses. */
  rex::memory::GuestMembase membase_;
  u32 app_id_;
};

class AppManager {
 public:
  /**
   * Adds an app, whose ID must not be registered yet.
   *
   * @param app The app, owned by the manager afterwards.
   */
  REX_XAM_API void RegisterApp(std::unique_ptr<XAMApplet> app);

  /**
   * Sends a message to the app with the given ID.
   *
   * @param app_id The ID of the receiving app.
   * @param buffer_ptr The guest address of the message buffer.
   * @return X_E_NOTFOUND when no app has the ID, else the result of the app.
   */
  REX_XAM_API rex::kernel::X_HRESULT DispatchMessageSync(u32 app_id, u32 message, u32 buffer_ptr,
                                                         u32 buffer_length);
  /**
   * Sends an asynchronous message to the app with the given ID, delivered through
   * DispatchMessageSync.
   *
   * @param app_id The ID of the receiving app.
   * @param buffer_ptr The guest address of the message buffer.
   * @return X_E_NOTFOUND when no app has the ID, else the result of the app.
   */
  REX_XAM_API rex::kernel::X_HRESULT DispatchMessageAsync(u32 app_id, u32 message, u32 buffer_ptr,
                                                          u32 buffer_length);

 private:
  std::vector<std::unique_ptr<XAMApplet>> apps_;
  std::unordered_map<u32, XAMApplet*> app_lookup_;
};

}  // namespace rex::xam
