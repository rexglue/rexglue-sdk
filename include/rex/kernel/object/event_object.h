/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/object/event_object.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @remarks     Parts derived from Xenia (Ben Vanik, 2022).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2015).
 * @remarks     Parts derived from Xenia (Ben Vanik, 2020).
 */

#pragma once

#include <memory>

#include <rex/assert.h>
#include <rex/kernel/object/object.h>
#include <rex/kernel/types/list.h>
#include <rex/kernel/types/memory.h>
#include <rex/kernel/types/status.h>
#include <rex/kernel/types/thread.h>
#include <rex/module.h>
#include <rex/platform.h>
#include <rex/platform/sync.h>
#include <rex/platform/thread.h>
#include <rex/platform/timer.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::kernel {

struct X_KEVENT {
  X_DISPATCH_HEADER header;
};
REX_STATIC_ASSERT_SIZE(X_KEVENT, 0x10);

class IGuestEvent {
 public:
  virtual ~IGuestEvent() = default;

  /**
   * @return The previous state as the kernel reports it, always 1.
   */
  virtual i32 Set() = 0;

  /**
   * @return The previous state as the kernel reports it, always 1.
   */
  virtual i32 Reset() = 0;
};

struct EventObject : IGuestEvent {
  /**
   * @param manual_reset_in True for a notification event, false for a synchronization event.
   * @param initial_state True when the event starts signaled.
   */
  REX_KERNEL_API EventObject(bool manual_reset_in, bool initial_state);

  /**
   * @param header The dispatch header of a title event, whose type and signal state initialize the
   * object.
   */
  REX_KERNEL_API explicit EventObject(const X_DISPATCH_HEADER& header);

  /** True for a notification event, false for a synchronization event that resets on a wait. */
  bool manual_reset = false;
  /** Host event that carries the signal state and wakes waiters. */
  PropagateConst<std::unique_ptr<rex::platform::Event>> event;

  /**
   * Reads the type and the live state of an event.
   *
   * @param out_type Receives 0 for a manual reset event and 1 otherwise, may be null.
   * @param out_state Receives 1 when signaled and 0 otherwise, may be null.
   */
  REX_KERNEL_API void Query(u32* out_type, u32* out_state);

  REX_KERNEL_API i32 Set() override;

  /**
   * Signals an event and releases the current waiters.
   *
   * @return The previous state as the kernel reports it, always 1.
   */
  REX_KERNEL_API i32 Pulse();

  REX_KERNEL_API i32 Reset() override;
};

}  // namespace rex::kernel
