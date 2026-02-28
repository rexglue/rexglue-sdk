/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/kernel/xboxkrnl/debug_monitor.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(kernel_pix, false, "Kernel", "Enable PIX debugging support");

namespace rex::kernel::xboxkrnl {

enum class DebugMonitorCommand {
  PIXCommandResult = 27,
  SetPIXCallback = 28,
  Unknown66 = 66,
  Unknown89 = 89,
  Unknown94 = 94,
};

void KeDebugMonitorCallback(PPCContext* ppc_context, rex::system::KernelState* kernel_state) {
  auto id = static_cast<DebugMonitorCommand>(ppc_context->r[3] & 0xFFFFFFFFu);
  auto arg = static_cast<uint32_t>(ppc_context->r[4] & 0xFFFFFFFFu);

  REXKRNL_DEBUG("KeDebugMonitorCallback({}, {:08X})", static_cast<uint32_t>(id), arg);

  if (!REXCVAR_GET(kernel_pix)) {
    ppc_context->r[3] = static_cast<uint64_t>(-1);
    return;
  }

  switch (id) {
    case DebugMonitorCommand::PIXCommandResult: {
      auto s = kernel_state->memory()->TranslateVirtual<const char*>(arg);
      rex::debug::DebugPrint("{}\n", s);
      REXKRNL_DEBUG("PIX command result: {}\n", s);
      ppc_context->r[3] = 0;
      break;
    }
    case DebugMonitorCommand::SetPIXCallback:
      // TODO: Implement PIX callback if needed
      ppc_context->r[3] = 0;
      break;
    case DebugMonitorCommand::Unknown66: {
      struct callback_info {
        rex::be<uint32_t> callback_fn;
        rex::be<uint32_t> callback_arg;  // D3D device object?
      };
      auto cbi = kernel_state->memory()->TranslateVirtual<callback_info*>(arg);
      ppc_context->r[3] = 0;
      break;
    }
    case DebugMonitorCommand::Unknown89:
      // arg = function pointer?
      ppc_context->r[3] = 0;
      break;
    case DebugMonitorCommand::Unknown94:
      ppc_context->r[3] = 0;
      break;
    default:
      ppc_context->r[3] = static_cast<uint64_t>(-1);
      break;
  }
}

}  // namespace rex::kernel::xboxkrnl
