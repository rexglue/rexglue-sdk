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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/assert.h>
#include <rex/export.h>
#include <rex/memory/fourcc.h>
#include <rex/platform/thread.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xstatus.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** Value StashHandle writes to wait_list_flink to mark a dispatch header that holds a handle. */
constexpr memory::FourCCT kStashSignature = memory::make_fourcc('R', 'E', 'X', '\0');

/** Header at the start of every waitable kernel object. */
struct X_DISPATCH_HEADER {
  struct {
    u8 type; /**< Object type, 1 of `X_DISPATCHER_FLAGS`. */

    union {
      /** Shares its byte with absolute, npx_irql and signalling. */
      u8 abandoned;
      /** Holds the spin count divided by 256 in a critical section. */
      u8 absolute;
      u8 npx_irql;   /**< No code reads it. */
      u8 signalling; /**< No code reads it. */
    };
    union {
      u8 size; /**< Shares its byte with hand and process_type. */
      u8 hand; /**< No code reads it. */
      /** Process type a timer was initialized with by KeInitializeTimerEx. */
      u8 process_type;
    };
    union {
      u8 inserted;     /**< Shares its byte with debug_active and dpc_active. */
      u8 debug_active; /**< No code reads it. */
      u8 dpc_active;   /**< No code reads it. */
    };
  };

  be_u32 signal_state;    /**< Signal count of the object. */
  be_u32 wait_list_flink; /**< First entry of the wait list. */
  be_u32 wait_list_blink; /**< Last entry of the wait list. */
};
REX_STATIC_ASSERT_SIZE(X_DISPATCH_HEADER, 0x10);

/** The object body follows this header. */
struct X_OBJECT_HEADER {
  be_u32 pointer_count;   /**< Reference count of the object, zeroed when the block is allocated. */
  be_u32 handle_count;    /**< Handle count of the object, zeroed when the block is allocated. */
  be_u32 object_type_ptr; /**< Guest address of the X_OBJECT_TYPE of the object. */
  be_u16 flags;           /**< Zero, no object flag is set. */
  u8 hash_index;          /**< Zero when the block is allocated. */
  u8 pad_15;
};
static_assert(offsetof(X_OBJECT_HEADER, object_type_ptr) == 0x8);
static_assert(offsetof(X_OBJECT_HEADER, flags) == 0xC);
REX_STATIC_ASSERT_SIZE(X_OBJECT_HEADER, 0x10);

/** Guest object type descriptor. */
struct X_OBJECT_TYPE {
  be_u32 constructor; /**< Reserved. */
  be_u32 destructor;  /**< Reserved. */
  be_u32 unk_08;      /**< Reserved. */
  be_u32 unk_0c;      /**< Reserved. */
  be_u32 unk_10;      /**< Reserved. */
  be_u32 unk_14;      /**< Reserved. */
  be_u32 pool_tag;    /**< Four character pool tag of the object type. */
};
REX_STATIC_ASSERT_SIZE(X_OBJECT_TYPE, 0x1C);

/** Kinds of kernel object a handle can name. */
enum class ObjectKind : u8 {
  kEvent,          /**< Selects `EventObject` in `ObjectData`. */
  kSemaphore,      /**< Selects `SemaphoreObject` in `ObjectData`. */
  kMutant,         /**< Selects `MutantObject` in `ObjectData`. */
  kTimer,          /**< Selects `TimerObject` in `ObjectData`. */
  kFile,           /**< Selects `FileObject` in `ObjectData`. */
  kSocket,         /**< Selects `SocketObject` in `ObjectData`. */
  kNotifyListener, /**< Selects `NotifyListenerObject` in `ObjectData`. */
  kIOCompletion,   /**< Selects `IOCompletionObject` in `ObjectData`. */
  kEnumerator,     /**< Selects `EnumeratorObject` in `ObjectData`. */
  kSymbolicLink,   /**< Selects `SymbolicLinkObject` in `ObjectData`. */
  kModule,         /**< Selects `ModuleObject` in `ObjectData`. */
  kThread,         /**< Selects `ThreadRef` in `ObjectData`. */
  kSession,        /**< Selects `SessionObject` in `ObjectData`. */
};

/** The value added to a table slot index, shifted left by 2, to form a handle. */
inline constexpr u32 kHandleBase = 0xF8000000;

/**
 * Writes the guest header stash that Ke exports use to find the handle.
 *
 * @param header The dispatch header inside a guest object.
 * @param handle The handle to store.
 */
inline void StashHandle(X_DISPATCH_HEADER* header, u32 handle) {
  header->wait_list_flink = kStashSignature;
  header->wait_list_blink = handle;
}

/**
 * Converts a wait timeout in 100 nanosecond ticks to milliseconds.
 *
 * @param timeout_ticks A negative relative timeout, a positive absolute system time, or 0.
 * @return The time until the timeout, 0 when it has passed.
 */
REX_RUNTIME_API std::chrono::milliseconds TimeoutFromTicks(i64 timeout_ticks);

/**
 * Converts a delay interval in 100 nanosecond ticks to microseconds, scaled by the guest clock.
 * The result is rounded up and never passes through milliseconds.
 *
 * @param delay_ticks A negative relative interval, a positive absolute system time, or 0.
 * @return The time until the interval ends, 0 when it has passed.
 */
REX_RUNTIME_API std::chrono::microseconds DelayFromTicks(i64 delay_ticks);

/**
 * Reads the object name out of an X_OBJECT_ATTRIBUTES.
 *
 * @param memory The memory that holds the attributes and the name.
 * @param obj_attributes_ptr The guest address of the attributes, or 0.
 * @return The name, empty when the attributes or the name pointer are null.
 */
REX_RUNTIME_API std::string NameFromAttributes(runtime::Memory* memory, u32 obj_attributes_ptr);

template <typename T>
class Pinned;

/**
 * Waits on the object a handle names. A kind without a wait handle succeeds at once.
 *
 * @param runtime The runtime the call runs in.
 * @param handle The handle, pseudo handles are translated.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return X_STATUS_SUCCESS, X_STATUS_USER_APC, X_STATUS_TIMEOUT, X_STATUS_ABANDONED_WAIT_0, or
 * X_STATUS_INVALID_HANDLE.
 */
REX_RUNTIME_API X_STATUS Wait(Runtime& runtime, X_HANDLE handle, u32 wait_reason,
                              u32 processor_mode, u32 alertable, u64* opt_timeout);

/**
 * Waits on several objects.
 *
 * @param runtime The runtime the call runs in.
 * @param handles The handles.
 * @param wait_type Nonzero to wait for any object, 0 to wait for all.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The index of the signaled object for a wait on any, or the status as Wait does, or
 * X_STATUS_INVALID_PARAMETER when a handle is invalid.
 */
REX_RUNTIME_API X_STATUS WaitMultiple(Runtime& runtime, std::span<const X_HANDLE> handles,
                                      u32 wait_type, u32 wait_reason, u32 processor_mode,
                                      u32 alertable, u64* opt_timeout);

/**
 * Signals one object and waits on another as a single operation.
 *
 * @param runtime The runtime the call runs in.
 * @param signal_handle The handle of the object to signal.
 * @param wait_handle The handle of the object to wait on.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The status as Wait returns it, X_STATUS_INVALID_HANDLE when either handle is invalid.
 */
REX_RUNTIME_API X_STATUS SignalAndWait(Runtime& runtime, X_HANDLE signal_handle,
                                       X_HANDLE wait_handle, u32 wait_reason, u32 processor_mode,
                                       u32 alertable, u64* opt_timeout);

}  // namespace rex::runtime

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::runtime::ObjectKind> : std::formatter<rex::u32> {
  /**
   * Formats the value.
   *
   * @param value The value.
   * @param ctx The format context.
   * @return The output iterator.
   */
  auto format(rex::runtime::ObjectKind value, std::format_context& ctx) const {
    return std::formatter<rex::u32>::format(static_cast<rex::u32>(value), ctx);
  }
};
