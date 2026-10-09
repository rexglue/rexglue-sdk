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
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xnotifylistener.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;

u32 XamNotifyCreateListener(u64 mask, u32 is_system, u32 max_version) {
  REX_ASSERT(max_version < 11);

  if (max_version > 10) {
    max_version = 10;
  }

  auto added = runtime::Current().object_table().Emplace<NotifyListenerObject>(AddOptions{}, mask,
                                                                               max_version);
  if (!added) {
    return 0;
  }
  const X_HANDLE handle = *added;
  runtime::Current().notifier().RegisterNotifyListener(handle);

  REXLOG_DEBUG(
      kernel,
      "XamNotifyCreateListener(mask={:#018x}, is_system={}, max_version={}) -> handle={:08X}", mask,
      is_system, max_version, handle);

  return handle;
}

u32 XamNotifyCreateListener_entry(u64 mask, u32 max_version) {
  return XamNotifyCreateListener(mask, 0, max_version);
}

u32 XamNotifyCreateListenerInternal_entry(u64 mask, u32 is_system, u32 max_version) {
  return XamNotifyCreateListener(mask, is_system, max_version);
}

u32 XNotifyGetNext_entry(u32 handle, u32 match_id, mapped_u32 id_ptr, mapped_u32 param_ptr) {
  if (param_ptr) {
    *param_ptr = 0;
  }

  if (!id_ptr) {
    return 0;
  }
  *id_ptr = 0;

  auto listener = runtime::Current().object_table().Get<NotifyListenerObject>(handle);
  if (!listener) {
    return 0;
  }

  bool dequeued = false;
  u32 id = 0;
  u32 param = 0;
  if (match_id) {
    id = match_id;
    dequeued = NotifyListenerDequeueByID(*listener, match_id, &param);
  } else {
    dequeued = NotifyListenerDequeue(*listener, &id, &param);
  }

  *id_ptr = dequeued ? id : 0;
  // param_ptr may be null.
  if (param_ptr) {
    *param_ptr = dequeued ? param : 0;
  }

  if (dequeued) {
    REXLOG_DEV(kernel, "XNotifyGetNext({:08X}, {:08X}) -> id={:#x}, param={}",
               static_cast<u32>(handle), static_cast<u32>(match_id), id, param);
  }
  return dequeued ? 1 : 0;
}

u32 XNotifyDelayUI_entry([[maybe_unused]] u32 delay_ms) {
  return 0;
}

void XNotifyPositionUI_entry([[maybe_unused]] u32 position) {}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamNotifyCreateListener, rex::kernel::xam::XamNotifyCreateListener_entry)
REX_EXPORT(__imp__XamNotifyCreateListenerInternal,
           rex::kernel::xam::XamNotifyCreateListenerInternal_entry)
REX_EXPORT(__imp__XNotifyGetNext, rex::kernel::xam::XNotifyGetNext_entry)
REX_EXPORT(__imp__XNotifyDelayUI, rex::kernel::xam::XNotifyDelayUI_entry)
REX_EXPORT(__imp__XNotifyPositionUI, rex::kernel::xam::XNotifyPositionUI_entry)

REX_EXPORT_STUB(__imp__XNotifyBroadcast);
REX_EXPORT_STUB(__imp__XNotifyQueueUI);
REX_EXPORT_STUB(__imp__XNotifyQueueUIEx);
REX_EXPORT_STUB(__imp__XNotifyRegisterArea);
REX_EXPORT_STUB(__imp__XNotifyUIGetOptions);
REX_EXPORT_STUB(__imp__XNotifyUISetOptions);
REX_EXPORT_STUB(__imp__XamNotifyCreateListenerRangeInternal);
REX_EXPORT_STUB(__imp__XamNotifyDelayUIInternal);
