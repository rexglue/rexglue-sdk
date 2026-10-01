#pragma once
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

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/platform/thread.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::system {

constexpr memory::fourcc_t kXObjSignature = memory::make_fourcc('R', 'E', 'X', '\0');

class KernelState;

// https://www.nirsoft.net/kernel_struct/vista/DISPATCHER_HEADER.html
typedef struct {
  struct {
    uint8_t type;

    union {
      uint8_t abandoned;
      uint8_t absolute;
      uint8_t npx_irql;
      uint8_t signalling;
    };
    union {
      uint8_t size;
      uint8_t hand;
      uint8_t process_type;
    };
    union {
      uint8_t inserted;
      uint8_t debug_active;
      uint8_t dpc_active;
    };
  };

  rex::be<uint32_t> signal_state;
  rex::be<uint32_t> wait_list_flink;
  rex::be<uint32_t> wait_list_blink;
} X_DISPATCH_HEADER;
static_assert_size(X_DISPATCH_HEADER, 0x10);

// https://www.nirsoft.net/kernel_struct/vista/OBJECT_HEADER.html
struct X_OBJECT_HEADER {
  rex::be<uint32_t> pointer_count;
  union {
    rex::be<uint32_t> handle_count;
    rex::be<uint32_t> next_to_free;
  };
  uint8_t name_info_offset;
  uint8_t handle_info_offset;
  uint8_t quota_info_offset;
  uint8_t flags;
  union {
    rex::be<uint32_t> object_create_info;  // X_OBJECT_CREATE_INFORMATION
    rex::be<uint32_t> quota_block_charged;
  };
  rex::be<uint32_t> object_type_ptr;  // -0x8 POBJECT_TYPE
  rex::be<uint32_t> unk_04;           // -0x4

  // Object lives after this header.
  // (There's actually a body field here which is the object itself)
};

// https://www.nirsoft.net/kernel_struct/vista/OBJECT_CREATE_INFORMATION.html
struct X_OBJECT_CREATE_INFORMATION {
  rex::be<uint32_t> attributes;                  // 0x0
  rex::be<uint32_t> root_directory_ptr;          // 0x4
  rex::be<uint32_t> parse_context_ptr;           // 0x8
  rex::be<uint32_t> probe_mode;                  // 0xC
  rex::be<uint32_t> paged_pool_charge;           // 0x10
  rex::be<uint32_t> non_paged_pool_charge;       // 0x14
  rex::be<uint32_t> security_descriptor_charge;  // 0x18
  rex::be<uint32_t> security_descriptor;         // 0x1C
  rex::be<uint32_t> security_qos_ptr;            // 0x20

  // Security QoS here (SECURITY_QUALITY_OF_SERVICE) too!
};

struct X_OBJECT_TYPE {
  rex::be<uint32_t> constructor;  // 0x0
  rex::be<uint32_t> destructor;   // 0x4
  rex::be<uint32_t> unk_08;       // 0x8
  rex::be<uint32_t> unk_0C;       // 0xC
  rex::be<uint32_t> unk_10;       // 0x10
  rex::be<uint32_t> unk_14;       // 0x14 probably offset from ntobject to keobject
  rex::be<uint32_t> pool_tag;     // 0x18
};

/** Kinds of kernel object a handle can name. */
enum class ObjectKind : u8 {
  Event,
  Semaphore,
  Mutant,
  Timer,
  File,
  Socket,
  NotifyListener,
  IOCompletion,
  Enumerator,
  SymbolicLink,
  Module,
  Thread,
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
 * Converts a relative timeout in 100 nanosecond ticks to milliseconds.
 *
 * @param timeout_ticks A negative relative timeout, or 0.
 * @return The milliseconds. A positive absolute timeout asserts and returns 0.
 */
u32 TimeoutTicksToMs(i64 timeout_ticks);

/**
 * Reads the object name out of an X_OBJECT_ATTRIBUTES.
 *
 * @param memory The guest memory.
 * @param obj_attributes_ptr The guest address of the attributes, or 0.
 * @return The name, empty when the attributes or the name pointer are null.
 */
std::string NameFromAttributes(memory::Memory* memory, u32 obj_attributes_ptr);

template <typename T>
class Pinned;

/**
 * Waits on the object a handle names. A kind without a wait handle succeeds at once.
 *
 * @param kernel_state The kernel state.
 * @param handle The handle, pseudo handles are translated.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return X_STATUS_SUCCESS, X_STATUS_USER_APC, X_STATUS_TIMEOUT, X_STATUS_ABANDONED_WAIT_0, or
 * X_STATUS_INVALID_HANDLE.
 */
X_STATUS WaitOn(KernelState* kernel_state, X_HANDLE handle, u32 wait_reason, u32 processor_mode,
                u32 alertable, u64* opt_timeout);

/**
 * Waits on several objects.
 *
 * @param kernel_state The kernel state.
 * @param handles The handles.
 * @param wait_type Nonzero to wait for any object, 0 to wait for all.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The index of the signaled object for a wait on any, or the status as WaitOn does, or
 * X_STATUS_INVALID_PARAMETER when a handle is invalid.
 */
X_STATUS WaitOnMany(KernelState* kernel_state, std::span<const X_HANDLE> handles, u32 wait_type,
                    u32 wait_reason, u32 processor_mode, u32 alertable, u64* opt_timeout);

/**
 * Signals one object and waits on another as a single operation.
 *
 * @param kernel_state The kernel state.
 * @param signal_handle The handle of the object to signal.
 * @param wait_handle The handle of the object to wait on.
 * @param wait_reason The wait reason.
 * @param processor_mode The processor mode.
 * @param alertable Nonzero when a user APC ends the wait.
 * @param opt_timeout The timeout in 100 nanosecond ticks, null to wait forever.
 * @return The status as WaitOn returns it, X_STATUS_INVALID_HANDLE when either handle is invalid.
 */
X_STATUS SignalAndWaitOn(KernelState* kernel_state, X_HANDLE signal_handle, X_HANDLE wait_handle,
                         u32 wait_reason, u32 processor_mode, u32 alertable, u64* opt_timeout);

/**
 * Finds the handle of the object behind a guest dispatch header, creating the object on first use.
 *
 * @param kernel_state The kernel state.
 * @param native_ptr The host pointer to the dispatch header.
 * @param as_type The dispatcher type to create, -1 to read it from the header.
 * @return The handle, 0 when the type is not supported.
 */
X_HANDLE ResolveGuestObject(KernelState* kernel_state, void* native_ptr, i32 as_type = -1);

/**
 * Pins the object behind a guest dispatch header as its object struct.
 *
 * @tparam T The expected object struct.
 * @param kernel_state The kernel state.
 * @param native_ptr The host pointer to the dispatch header.
 * @param as_type The dispatcher type to create, -1 to read it from the header.
 * @return The pin, empty when the object is absent or holds another kind.
 */
template <typename T>
Pinned<T> ResolveGuest(KernelState* kernel_state, void* native_ptr, i32 as_type = -1);

/**
 * Constructs an object, backs it with a guest block when one is wanted, names it from the object
 * attributes and stashes its handle in the block.
 *
 * @tparam T The object struct to construct.
 * @tparam Args The constructor argument types of T.
 * @param kernel_state The kernel state.
 * @param out_handle Receives the handle.
 * @param attributes_ptr The guest address of the object attributes, or 0.
 * @param block_size The size of the guest block, 0 for none.
 * @param args The constructor arguments of T.
 * @return X_STATUS_SUCCESS, X_STATUS_NO_MEMORY, or the status of the table.
 */
template <typename T, typename... Args>
X_STATUS CreateWithBlock(KernelState* kernel_state, X_HANDLE* out_handle, u32 attributes_ptr,
                         u32 block_size, Args&&... args);

}  // namespace rex::system

/** Formats the enum as its underlying integer. */
template <>
struct std::formatter<rex::system::ObjectKind> : std::formatter<rex::u32> {
  /**
   * Formats the value.
   *
   * @param value The value.
   * @param ctx The format context.
   * @return The output iterator.
   */
  auto format(rex::system::ObjectKind value, std::format_context& ctx) const {
    return std::formatter<rex::u32>::format(static_cast<rex::u32>(value), ctx);
  }
};
