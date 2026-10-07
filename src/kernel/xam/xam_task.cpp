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
#include <rex/runtime/user_module.h>
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
using namespace rex::runtime::xam;

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

u32 XamTaskSchedule_entry(mapped_void callback, ppc_ptr_t<XTASK_MESSAGE> message,
                          mapped_u32 unknown, mapped_u32 handle_ptr) {
  // TODO(gibbed): figure out what this is for
  *handle_ptr = 12345;

  auto record = std::make_unique<ThreadRecord>(runtime::Current(), 0, 0, callback.guest_address(),
                                               message.guest_address(), 0, true);

  X_HANDLE task_thread = 0;
  auto launched = runtime::Current().thread_registry().Launch(std::move(record));
  if (launched) {
    task_thread = *launched;
  }
  X_STATUS result = ToXStatus(launched);

  if (XFAILED(result)) {
    // Failed!
    REX_FAIL(kernel, result, "XAM task creation failed: {:08X}", result);
  }
  (void)runtime::Current().object_table().Close(task_thread);

  REXLOG_DEBUG(kernel, "XAM task ({:08X}) scheduled asynchronously", callback.guest_address());

  return X_STATUS_SUCCESS;
}

u32 XamTaskShouldExit_entry(u32 r3) {
  return 0;
}

u32 XamTaskCloseHandle_entry(u32 handle) {
  REX_KERNEL_STUB("XamTaskCloseHandle({:#x})", static_cast<uint32_t>(handle));
  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamTaskSchedule, rex::kernel::xam::XamTaskSchedule_entry)
REX_EXPORT(__imp__XamTaskShouldExit, rex::kernel::xam::XamTaskShouldExit_entry)
REX_EXPORT(__imp__XamTaskCloseHandle, rex::kernel::xam::XamTaskCloseHandle_entry)

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
