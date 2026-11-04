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

#include "xam/xam_state.h"
#include "xam/xam_ui.h"
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/object/notify_listener_object.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

u32 XamNotifyCreateListener(u64 mask, u32 is_system, u32 max_version) {
  REX_ASSERT(max_version < 11);

  if (max_version > 10) {
    max_version = 10;
  }

  auto added = CurrentTitle().notifier.AddNotifyListener(mask, max_version);
  if (!added) {
    return 0;
  }
  const kernel::X_HANDLE handle = *added;

  REXXAM_DEBUG(
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

u32 XNotifyGetNext_entry(u32 handle, u32 match_id, MappedU32 id_ptr, MappedU32 param_ptr) {
  if (param_ptr) {
    *param_ptr = 0;
  }

  if (!id_ptr) {
    return 0;
  }
  *id_ptr = 0;

  auto listener = CurrentTitle().objects.Get<kernel::INotifyListenerQueue>(handle);
  if (!listener) {
    return 0;
  }

  bool dequeued = false;
  u32 id = 0;
  u32 param = 0;
  if (match_id) {
    id = match_id;
    dequeued = listener->DequeueByID(match_id, &param);
  } else {
    dequeued = listener->Dequeue(&id, &param);
  }

  *id_ptr = dequeued ? id : 0;
  if (param_ptr) {
    *param_ptr = dequeued ? param : 0;
  }

  if (dequeued) {
    REXXAM_DEV("XNotifyGetNext({:08X}, {:08X}) -> id={:#x}, param={}", static_cast<u32>(handle),
               static_cast<u32>(match_id), id, param);
  }
  return dequeued ? 1 : 0;
}

u32 XNotifyDelayUI_entry([[maybe_unused]] u32 delay_ms) {
  return 0;
}

void XNotifyPositionUI_entry([[maybe_unused]] u32 position) {}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamNotifyCreateListener, rex::xam::XamNotifyCreateListener_entry)
REX_XAM_BODY(__imp__XamNotifyCreateListenerInternal,
             rex::xam::XamNotifyCreateListenerInternal_entry)
REX_XAM_BODY(__imp__XNotifyGetNext, rex::xam::XNotifyGetNext_entry)
REX_XAM_BODY(__imp__XNotifyDelayUI, rex::xam::XNotifyDelayUI_entry)
REX_XAM_BODY(__imp__XNotifyPositionUI, rex::xam::XNotifyPositionUI_entry)
