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

#pragma once

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/app_manager.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

/** XAM app 0xFE, which serves content enumeration messages. */
class XAMApp : public runtime::App {
 public:
  /**
   * Creates the app.
   *
   * @param runtime The runtime the app serves.
   */
  REX_RUNTIME_API explicit XAMApp(Runtime& runtime);

  /**
   * Handles a message sent to the XAM app.
   *
   * @param message The message identifier.
   * @param buffer_ptr Guest address of the message buffer.
   * @param buffer_length Size of the message buffer in bytes.
   * @return The result code returned to the title.
   */
  REX_RUNTIME_API X_HRESULT DispatchMessageSync(u32 message, u32 buffer_ptr,
                                                u32 buffer_length) override;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
