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

#pragma once

#include <memory>

#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/object.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

/** Guest layout of a kernel event. */
struct X_KEVENT {
  X_DISPATCH_HEADER header; /**< Dispatch header. */
};
REX_STATIC_ASSERT_SIZE(X_KEVENT, 0x10);

/** The host state of an event object. */
struct EventObject {
  /**
   * @param manual_reset_in True for a notification event, false for a synchronization event.
   * @param initial_state True when the event starts signaled.
   */
  REX_RUNTIME_API EventObject(bool manual_reset_in, bool initial_state);

  /**
   * @param header The dispatch header of a title event, whose type and signal state initialize the
   * object.
   */
  REX_RUNTIME_API explicit EventObject(const X_DISPATCH_HEADER& header);

  /** True for a notification event, false for a synchronization event that resets on a wait. */
  bool manual_reset = false;
  /** Host event that carries the signal state and wakes waiters. */
  PropagateConst<std::unique_ptr<rex::platform::Event>> event;

/**
 * Reads the type and the live state of an event.
 *
 * @param event The event.
 * @param out_type Receives 0 for a manual reset event and 1 otherwise, may be null.
 * @param out_state Receives 1 when signaled and 0 otherwise, may be null.
 */
REX_RUNTIME_API void EventQuery(EventObject& event, u32* out_type, u32* out_state);

/**
 * Signals an event.
 *
 * @param event The event.
 * @return The previous state as the kernel reports it, always 1.
 */
REX_RUNTIME_API i32 EventSet(EventObject& event);

/**
 * Signals an event and releases the current waiters.
 *
 * @param event The event.
 * @return The previous state as the kernel reports it, always 1.
 */
REX_RUNTIME_API i32 EventPulse(EventObject& event);

/**
 * Clears an event.
 *
 * @param event The event.
 * @return The previous state as the kernel reports it, always 1.
 */
REX_RUNTIME_API i32 EventReset(EventObject& event);

}  // namespace rex::runtime
