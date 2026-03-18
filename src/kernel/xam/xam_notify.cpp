/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xnotifylistener.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

uint32_t xeXamNotifyCreateListener(uint64_t mask, uint32_t is_system, uint32_t max_version) {
  assert_true(max_version < 11);

  if (max_version > 10) {
    max_version = 10;
  }

  auto listener = object_ref<XNotifyListener>(new XNotifyListener(REX_KERNEL_STATE()));
  listener->Initialize(mask, max_version);

  // Handle ref is incremented, so return that.
  uint32_t handle = listener->handle();

  REXKRNL_DEBUG(
      "XamNotifyCreateListener(mask={:#018x}, is_system={}, max_version={}) -> handle={:08X}", mask,
      is_system, max_version, handle);

  return handle;
}

ppc_u32_result_t XamNotifyCreateListener_entry(ppc_u64_t mask, ppc_u32_t max_version) {
  return xeXamNotifyCreateListener(mask, 0, max_version);
}

ppc_u32_result_t XamNotifyCreateListenerInternal_entry(ppc_u64_t mask, ppc_u32_t is_system,
                                                       ppc_u32_t max_version) {
  return xeXamNotifyCreateListener(mask, is_system, max_version);
}

// https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgSignIn.cpp
ppc_u32_result_t XNotifyGetNext_entry(ppc_u32_t handle, ppc_u32_t match_id, ppc_pu32_t id_ptr,
                                      ppc_pu32_t param_ptr) {
  if (param_ptr) {
    *param_ptr = 0;
  }

  if (!id_ptr) {
    return 0;
  }
  *id_ptr = 0;

  // Grab listener.
  auto listener = REX_KERNEL_OBJECTS()->LookupObject<XNotifyListener>(handle);
  if (!listener) {
    return 0;
  }

  bool dequeued = false;
  uint32_t id = 0;
  uint32_t param = 0;
  if (match_id) {
    // Asking for a specific notification
    id = match_id;
    dequeued = listener->DequeueNotification(match_id, &param);
  } else {
    // Just get next.
    dequeued = listener->DequeueNotification(&id, &param);
  }

  *id_ptr = dequeued ? id : 0;
  // param_ptr may be null - 555307F0 Demo explicitly passes nullptr in the
  // code.
  // https://github.com/xenia-project/xenia/pull/1577
  if (param_ptr) {
    *param_ptr = dequeued ? param : 0;
  }

  if (dequeued) {
    REXKRNL_DEBUG("XNotifyGetNext({:08X}, {:08X}) -> id={:#x}, param={}", uint32_t(handle),
                  uint32_t(match_id), id, param);
  }
  return dequeued ? 1 : 0;
}

ppc_u32_result_t XNotifyDelayUI_entry(ppc_u32_t delay_ms) {
  // Ignored.
  return 0;
}

void XNotifyPositionUI_entry(ppc_u32_t position) {
  // Ignored.
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

XAM_EXPORT(__imp__XamNotifyCreateListener, rex::kernel::xam::XamNotifyCreateListener_entry)
XAM_EXPORT(__imp__XamNotifyCreateListenerInternal,
           rex::kernel::xam::XamNotifyCreateListenerInternal_entry)
XAM_EXPORT(__imp__XNotifyGetNext, rex::kernel::xam::XNotifyGetNext_entry)
XAM_EXPORT(__imp__XNotifyDelayUI, rex::kernel::xam::XNotifyDelayUI_entry)
XAM_EXPORT(__imp__XNotifyPositionUI, rex::kernel::xam::XNotifyPositionUI_entry)

XAM_EXPORT_STUB(__imp__XNotifyBroadcast);
XAM_EXPORT_STUB(__imp__XNotifyQueueUI);
XAM_EXPORT_STUB(__imp__XNotifyQueueUIEx);
XAM_EXPORT_STUB(__imp__XNotifyRegisterArea);
XAM_EXPORT_STUB(__imp__XNotifyUIGetOptions);
XAM_EXPORT_STUB(__imp__XNotifyUISetOptions);
XAM_EXPORT_STUB(__imp__XamNotifyCreateListenerRangeInternal);
XAM_EXPORT_STUB(__imp__XamNotifyDelayUIInternal);
