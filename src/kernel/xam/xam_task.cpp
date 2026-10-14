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

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/xthread.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <format>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

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

  auto thread = std::make_unique<KernelThread>(
      runtime::GetCurrent(), 0, 0, callback.GetGuestAddress(), message.GetGuestAddress(), 0, true);

  X_HANDLE task_thread = 0;
  auto launched = runtime::GetCurrent().GetThreadRegistry().Launch(std::move(thread));
  if (launched) {
    task_thread = *launched;
  }
  X_STATUS result = ToXStatus(launched);

  if (XFAILED(result)) {
    REX_FAIL(kernel, result, "XAM task creation failed: {:08X}", result);
  }
  runtime::CloseOwned(runtime::GetCurrent().GetObjectTable(), task_thread);

  REXLOG_DEBUG(kernel, "XAM task ({:08X}) scheduled asynchronously", callback.GetGuestAddress());

  return X_STATUS_SUCCESS;
}

u32 XamTaskShouldExit_entry([[maybe_unused]] u32 r3) {
  return 0;
}

u32 XamTaskCloseHandle_entry(u32 handle) {
  REX_KERNEL_STUB("XamTaskCloseHandle({:#x})", static_cast<u32>(handle));
  return X_STATUS_SUCCESS;
}

void XRegisterThreadNotifyRoutine_entry(GuestPtrT<runtime::X_THREAD_NOTIFICATION> notification,
                                        u32 register_notification) {
  auto& registry = runtime::GetCurrent().GetThreadRegistry();
  if (register_notification) {
    registry.RegisterThreadNotify(notification.GetGuestAddress());
  } else {
    registry.RemoveThreadNotify(notification.GetGuestAddress());
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamTaskSchedule, rex::kernel::xam::XamTaskSchedule_entry)
REX_EXPORT(__imp__XamTaskShouldExit, rex::kernel::xam::XamTaskShouldExit_entry)
REX_EXPORT(__imp__XamTaskCloseHandle, rex::kernel::xam::XamTaskCloseHandle_entry)
REX_EXPORT(__imp__XRegisterThreadNotifyRoutine,
           rex::kernel::xam::XRegisterThreadNotifyRoutine_entry)

REX_EXPORT_STUB(__imp__XamTaskCancel);
REX_EXPORT_STUB(__imp__XamTaskCancelWaitAndCloseWaitTask);
REX_EXPORT_STUB(__imp__XamTaskCreateQueue);
REX_EXPORT_STUB(__imp__XamTaskCreateQueueEx);
REX_EXPORT_STUB(__imp__XamTaskGetAttributes);
REX_EXPORT_STUB(__imp__XamTaskGetCompletionStatus);
REX_EXPORT_STUB(__imp__XamTaskGetCurrentTask);
REX_EXPORT_STUB(__imp__XamTaskGetStatus);
REX_EXPORT_STUB(__imp__XamTaskModify);
REX_EXPORT_STUB(__imp__XamTaskQueryProperty);
REX_EXPORT_STUB(__imp__XamTaskReschedule);
REX_EXPORT_STUB(__imp__XamTaskSetCancelSubTasks);
REX_EXPORT_STUB(__imp__XamTaskWaitOnCompletion);
