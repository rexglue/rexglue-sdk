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
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/thread.h>
#include <rex/ppc/call.h>
#include "xam/xam_ui.h"
#include <rex/kernel/types/thread.h>
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include "xam/stub_log.h"

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <format>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

struct XTASK_MESSAGE {
  be_u32 unknown_00;
  be_u32 unknown_04;
  be_u32 unknown_08;
  be_u32 callback_arg_ptr;
  be_u32 event_handle;
  be_u32 unknown_14;
  be_u32 task_handle;
};
REX_STATIC_ASSERT_SIZE(XTASK_MESSAGE, 0x1C);

u32 XamTaskSchedule_entry(MappedVoid callback, GuestPtrT<XTASK_MESSAGE> message,
                          [[maybe_unused]] MappedU32 unknown, MappedU32 handle_ptr) {
  *handle_ptr = 12345;

  kernel::X_HANDLE task_thread = 0;
  auto launched = CurrentTitle().threads.CreateTaskThread(callback.GetGuestAddress(),
                                                          message.GetGuestAddress());
  if (launched) {
    task_thread = *launched;
  }
  kernel::X_STATUS result = kernel::ToXStatus(launched);

  if (kernel::XFAILED(result)) {
    REX_FAIL(xam, result, "XAM task creation failed: {:08X}", result);
  }
  kernel::CloseOwned(CurrentTitle().objects, task_thread);

  REXLOG_DEBUG(xam, "XAM task ({:08X}) scheduled asynchronously", callback.GetGuestAddress());

  return kernel::X_STATUS_SUCCESS;
}

u32 XamTaskShouldExit_entry([[maybe_unused]] u32 r3) {
  return 0;
}

u32 XamTaskCloseHandle_entry(u32 handle) {
  REX_XAM_STUB("XamTaskCloseHandle({:#x})", static_cast<u32>(handle));
  return kernel::X_STATUS_SUCCESS;
}

void XRegisterThreadNotifyRoutine_entry(GuestPtrT<kernel::X_THREAD_NOTIFICATION> notification,
                                        u32 register_notification) {
  auto& registry = CurrentTitle().threads;
  if (register_notification) {
    registry.RegisterThreadNotify(notification.GetGuestAddress());
  } else {
    registry.UnregisterThreadNotify(notification.GetGuestAddress());
  }
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamTaskSchedule, rex::xam::XamTaskSchedule_entry)
REX_XAM_BODY(__imp__XamTaskShouldExit, rex::xam::XamTaskShouldExit_entry)
REX_XAM_BODY(__imp__XamTaskCloseHandle, rex::xam::XamTaskCloseHandle_entry)
REX_XAM_BODY(__imp__XRegisterThreadNotifyRoutine, rex::xam::XRegisterThreadNotifyRoutine_entry)
