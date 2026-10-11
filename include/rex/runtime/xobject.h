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
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <rex/assert.h>
#include <rex/macros.h>
#include <rex/platform/thread.h>
#include <rex/runtime/xmemory.h>
#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

constexpr memory::fourcc_t kXObjSignature = memory::make_fourcc('R', 'E', 'X', '\0');

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
  /** Offset 0x0. Reference count of the object, zeroed when the block is allocated. */
  be_u32 pointer_count;
  union {
    /** Offset 0x4. Shares its dword with next_to_free, zeroed when the block is allocated. */
    be_u32 handle_count;
    be_u32 next_to_free; /**< Offset 0x4. Shares its dword with handle_count. */
  };
  u8 name_info_offset;   /**< Offset 0x8. Zero, no name info block precedes the header. */
  u8 handle_info_offset; /**< Offset 0x9. Zero, no handle info block precedes the header. */
  u8 quota_info_offset;  /**< Offset 0xA. Zero, no quota info block precedes the header. */
  u8 flags;              /**< Offset 0xB. Zero, no object flag is set. */
  union {
    be_u32 object_create_info;  /**< Points to an X_OBJECT_CREATE_INFORMATION. */
    be_u32 quota_block_charged; /**< Offset 0xC. Shares its dword with object_create_info. */
  };
  be_u32 object_type_ptr; /**< Offset -0x8. POBJECT_TYPE. */
  be_u32 unk_04;          /**< Offset -0x4. */
};

/** A SECURITY_QUALITY_OF_SERVICE follows the last field. */
struct X_OBJECT_CREATE_INFORMATION {
  be_u32 attributes;                 /**< Offset 0x0. */
  be_u32 root_directory_ptr;         /**< Offset 0x4. */
  be_u32 parse_context_ptr;          /**< Offset 0x8. */
  be_u32 probe_mode;                 /**< Offset 0xC. */
  be_u32 paged_pool_charge;          /**< Offset 0x10. */
  be_u32 non_paged_pool_charge;      /**< Offset 0x14. */
  be_u32 security_descriptor_charge; /**< Offset 0x18. */
  be_u32 security_descriptor;        /**< Offset 0x1C. */
  be_u32 security_qos_ptr;           /**< Offset 0x20. */
};

/** Guest object type descriptor. */
struct X_OBJECT_TYPE {
  be_u32 constructor;
  be_u32 destructor;
  be_u32 unk_08;
  be_u32 unk_0C;
  be_u32 unk_10;
  be_u32 unk_14;
  be_u32 pool_tag;
};
REX_STATIC_ASSERT_SIZE(X_OBJECT_TYPE, 0x1C);

/** Kinds of kernel object a handle can name. */
enum class ObjectKind : u8 {
  Event,          /**< Selects `EventObject` in `ObjectData`. */
  Semaphore,      /**< Selects `SemaphoreObject` in `ObjectData`. */
  Mutant,         /**< Selects `MutantObject` in `ObjectData`. */
  Timer,          /**< Selects `TimerObject` in `ObjectData`. */
  File,           /**< Selects `FileObject` in `ObjectData`. */
  Socket,         /**< Selects `SocketObject` in `ObjectData`. */
  NotifyListener, /**< Selects `NotifyListenerObject` in `ObjectData`. */
  IOCompletion,   /**< Selects `IOCompletionObject` in `ObjectData`. */
  Enumerator,     /**< Selects `EnumeratorObject` in `ObjectData`. */
  SymbolicLink,   /**< Selects `SymbolicLinkObject` in `ObjectData`. */
  Module,         /**< Selects `ModuleObject` in `ObjectData`. */
  Thread,         /**< Selects `ThreadRef` in `ObjectData`. */
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
  header->wait_list_flink = kXObjSignature;
  header->wait_list_blink = handle;
}

/**
 * Converts a wait timeout in 100 nanosecond ticks to milliseconds.
 *
 * @param timeout_ticks A negative relative timeout, a positive absolute system time, or 0.
 * @return The milliseconds until the timeout, 0 when it has passed.
 */
REX_RUNTIME_API u32 TimeoutTicksToMs(i64 timeout_ticks);

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
REX_RUNTIME_API X_STATUS WaitOn(Runtime& runtime, X_HANDLE handle, u32 wait_reason,
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
 * @return The index of the signaled object for a wait on any, or the status as WaitOn does, or
 * X_STATUS_INVALID_PARAMETER when a handle is invalid.
 */
REX_RUNTIME_API X_STATUS WaitOnMany(Runtime& runtime, std::span<const X_HANDLE> handles,
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
 * @return The status as WaitOn returns it, X_STATUS_INVALID_HANDLE when either handle is invalid.
 */
REX_RUNTIME_API X_STATUS SignalAndWaitOn(Runtime& runtime, X_HANDLE signal_handle,
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
