/**
 * @file        runtime/xtypes.h
 * @brief       Core type definitions (X_STATUS, X_RESULT, X_HRESULT)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex {

/** Value the title uses to name a kernel object. */
using X_HANDLE = u32;
inline constexpr X_HANDLE X_INVALID_HANDLE_VALUE = static_cast<X_HANDLE>(-1);

/** NT status code type, holding STATUS_* values. */
using X_STATUS = u32;
[[nodiscard]] constexpr bool XSUCCEEDED(X_STATUS s) {
  return (s & 0xC0000000) == 0;
}
[[nodiscard]] constexpr bool XFAILED(X_STATUS s) {
  return !XSUCCEEDED(s);
}
inline constexpr X_STATUS X_STATUS_SUCCESS = 0x00000000;
inline constexpr X_STATUS X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
inline constexpr X_STATUS X_STATUS_USER_APC = 0x000000C0;
inline constexpr X_STATUS X_STATUS_ALERTED = 0x00000101;
inline constexpr X_STATUS X_STATUS_TIMEOUT = 0x00000102;
inline constexpr X_STATUS X_STATUS_PENDING = 0x00000103;
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_EXISTS = 0x40000000;
inline constexpr X_STATUS X_STATUS_TIMER_RESUME_IGNORED = 0x40000025;
inline constexpr X_STATUS X_STATUS_BUFFER_OVERFLOW = 0x80000005;
inline constexpr X_STATUS X_STATUS_NO_MORE_FILES = 0x80000006;
inline constexpr X_STATUS X_STATUS_UNSUCCESSFUL = 0xC0000001;
inline constexpr X_STATUS X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
inline constexpr X_STATUS X_STATUS_INVALID_INFO_CLASS = 0xC0000003;
inline constexpr X_STATUS X_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
inline constexpr X_STATUS X_STATUS_ACCESS_VIOLATION = 0xC0000005;
inline constexpr X_STATUS X_STATUS_INVALID_HANDLE = 0xC0000008;
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER = 0xC000000D;
inline constexpr X_STATUS X_STATUS_NO_SUCH_FILE = 0xC000000F;
inline constexpr X_STATUS X_STATUS_END_OF_FILE = 0xC0000011;
inline constexpr X_STATUS X_STATUS_NO_MEMORY = 0xC0000017;
inline constexpr X_STATUS X_STATUS_ALREADY_COMMITTED = 0xC0000021;
inline constexpr X_STATUS X_STATUS_ACCESS_DENIED = 0xC0000022;
inline constexpr X_STATUS X_STATUS_BUFFER_TOO_SMALL = 0xC0000023;
inline constexpr X_STATUS X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_INVALID = 0xC0000033;
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
inline constexpr X_STATUS X_STATUS_SHARING_VIOLATION = 0xC0000043;
inline constexpr X_STATUS X_STATUS_INVALID_PAGE_PROTECTION = 0xC0000045;
inline constexpr X_STATUS X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
inline constexpr X_STATUS X_STATUS_THREAD_IS_TERMINATING = 0xC000004B;
inline constexpr X_STATUS X_STATUS_PROCESS_IS_TERMINATING = 0xC000010A;
inline constexpr X_STATUS X_STATUS_PROCEDURE_NOT_FOUND = 0xC000007A;
inline constexpr X_STATUS X_STATUS_INSUFFICIENT_RESOURCES = 0xC000009A;
inline constexpr X_STATUS X_STATUS_NOT_SAME_DEVICE = 0xC00000D4;
inline constexpr X_STATUS X_STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
inline constexpr X_STATUS X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
inline constexpr X_STATUS X_STATUS_NOT_SUPPORTED = 0xC00000BB;
inline constexpr X_STATUS X_STATUS_NOT_A_DIRECTORY = 0xC0000103;
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_1 = 0xC00000EF;
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_2 = 0xC00000F0;
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_3 = 0xC00000F1;
inline constexpr X_STATUS X_STATUS_DLL_NOT_FOUND = 0xC0000135;
inline constexpr X_STATUS X_STATUS_ENTRYPOINT_NOT_FOUND = 0xC0000139;
inline constexpr X_STATUS X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC000012B;
inline constexpr X_STATUS X_STATUS_MAPPED_ALIGNMENT = 0xC0000220;
inline constexpr X_STATUS X_STATUS_NOT_FOUND = 0xC0000225;
inline constexpr X_STATUS X_STATUS_DRIVER_ORDINAL_NOT_FOUND = 0xC0000262;
inline constexpr X_STATUS X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND = 0xC0000263;

/** Win32 error code type, holding ERROR_* values. */
using X_RESULT = u32;
inline constexpr i32 X_FACILITY_WIN32 = 0x0007;
[[nodiscard]] constexpr X_RESULT X_RESULT_FROM_WIN32(u32 x) {
  return x;
}

inline constexpr X_RESULT X_ERROR_SUCCESS = X_RESULT_FROM_WIN32(0x00000000);
inline constexpr X_RESULT X_ERROR_FILE_NOT_FOUND = X_RESULT_FROM_WIN32(0x00000002);
inline constexpr X_RESULT X_ERROR_PATH_NOT_FOUND = X_RESULT_FROM_WIN32(0x00000003);
inline constexpr X_RESULT X_ERROR_ACCESS_DENIED = X_RESULT_FROM_WIN32(0x00000005);
inline constexpr X_RESULT X_ERROR_INVALID_HANDLE = X_RESULT_FROM_WIN32(0x00000006);
inline constexpr X_RESULT X_ERROR_NOT_ENOUGH_MEMORY = X_RESULT_FROM_WIN32(0x00000008);
inline constexpr X_RESULT X_ERROR_NO_MORE_FILES = X_RESULT_FROM_WIN32(0x00000012);
inline constexpr X_RESULT X_ERROR_INVALID_PARAMETER = X_RESULT_FROM_WIN32(0x00000057);
inline constexpr X_RESULT X_ERROR_INSUFFICIENT_BUFFER = X_RESULT_FROM_WIN32(0x0000007A);
inline constexpr X_RESULT X_ERROR_INVALID_NAME = X_RESULT_FROM_WIN32(0x0000007B);
inline constexpr X_RESULT X_ERROR_BAD_ARGUMENTS = X_RESULT_FROM_WIN32(0x000000A0);
inline constexpr X_RESULT X_ERROR_BUSY = X_RESULT_FROM_WIN32(0x000000AA);
inline constexpr X_RESULT X_ERROR_ALREADY_EXISTS = X_RESULT_FROM_WIN32(0x000000B7);
inline constexpr X_RESULT X_ERROR_IO_INCOMPLETE = X_RESULT_FROM_WIN32(0x000003E4);
inline constexpr X_RESULT X_ERROR_IO_PENDING = X_RESULT_FROM_WIN32(0x000003E5);
inline constexpr X_RESULT X_ERROR_DEVICE_NOT_CONNECTED = X_RESULT_FROM_WIN32(0x0000048F);
inline constexpr X_RESULT X_ERROR_NOT_FOUND = X_RESULT_FROM_WIN32(0x00000490);
inline constexpr X_RESULT X_ERROR_CANCELLED = X_RESULT_FROM_WIN32(0x000004C7);
inline constexpr X_RESULT X_ERROR_NOT_LOGGED_ON = X_RESULT_FROM_WIN32(0x000004DD);
inline constexpr X_RESULT X_ERROR_NO_SUCH_USER = X_RESULT_FROM_WIN32(0x00000525);
inline constexpr X_RESULT X_ERROR_FUNCTION_FAILED = X_RESULT_FROM_WIN32(0x0000065B);
inline constexpr X_RESULT X_ERROR_EMPTY = X_RESULT_FROM_WIN32(0x000010D2);

/** HRESULT code type. */
using X_HRESULT = u32;
[[nodiscard]] constexpr X_HRESULT X_HRESULT_FROM_WIN32(u32 x) {
  return static_cast<i32>(x) <= 0 ? x : ((x & 0xFFFF) | (X_FACILITY_WIN32 << 16) | 0x80000000);
}

inline constexpr X_HRESULT X_E_FALSE = 0x80000000;
inline constexpr X_HRESULT X_E_SUCCESS = X_HRESULT_FROM_WIN32(X_ERROR_SUCCESS);
inline constexpr X_HRESULT X_E_FAIL = 0x80004005;
inline constexpr X_HRESULT X_E_NO_MORE_FILES = X_HRESULT_FROM_WIN32(X_ERROR_NO_MORE_FILES);
inline constexpr X_HRESULT X_E_INVALIDARG = X_HRESULT_FROM_WIN32(X_ERROR_INVALID_PARAMETER);
inline constexpr X_HRESULT X_E_DEVICE_NOT_CONNECTED =
    X_HRESULT_FROM_WIN32(X_ERROR_DEVICE_NOT_CONNECTED);
inline constexpr X_HRESULT X_E_NOTFOUND = X_HRESULT_FROM_WIN32(X_ERROR_NOT_FOUND);
inline constexpr X_HRESULT X_E_NO_SUCH_USER = X_HRESULT_FROM_WIN32(X_ERROR_NO_SUCH_USER);

/** MEM_* flags used by NtAllocateVirtualMemory. */
enum X_MEM : u32 {
  X_MEM_COMMIT = 0x00001000,   /**< Commits the pages. */
  X_MEM_RESERVE = 0x00002000,  /**< Reserves the address range. */
  X_MEM_DECOMMIT = 0x00004000, /**< Decommits the pages and keeps the reservation. */
  X_MEM_RELEASE = 0x00008000,  /**< Releases the whole reservation. */
  X_MEM_FREE = 0x00010000,    /**< State NtQueryVirtualMemory reports for pages neither reserved nor
                                 committed. */
  X_MEM_PRIVATE = 0x00020000, /**< Type NtQueryVirtualMemory reports for every region. */
  X_MEM_RESET = 0x00080000,   /**< Valid only alone, NtAllocateVirtualMemory logs it as a stub. */
  X_MEM_TOP_DOWN = 0x00100000, /**< Allocates at the highest free address. */
  X_MEM_NOZERO = 0x00800000,   /**< Leaves the new pages unzeroed. */
  X_MEM_LARGE_PAGES =
      0x20000000,               /**< Backs the allocation with 64 KB pages instead of 4 KB pages. */
  X_MEM_HEAP = 0x40000000,      /**< No code reads it. */
  X_MEM_16MB_PAGES = 0x80000000 /**< Backs the allocation with 16 MB pages. */
};

/** PAGE_* flags used by NtAllocateVirtualMemory. */
enum X_PAGE : u16 {
  X_PAGE_NOACCESS = 0x00000001,          /**< Any access faults. */
  X_PAGE_READONLY = 0x00000002,          /**< Pages can be read. */
  X_PAGE_READWRITE = 0x00000004,         /**< Pages can be read and written. */
  X_PAGE_WRITECOPY = 0x00000008,         /**< No code reads it. */
  X_PAGE_EXECUTE = 0x00000010,           /**< Equivalent to X_PAGE_READONLY. */
  X_PAGE_EXECUTE_READ = 0x00000020,      /**< Equivalent to X_PAGE_READONLY. */
  X_PAGE_EXECUTE_READWRITE = 0x00000040, /**< Equivalent to X_PAGE_READWRITE. */
  X_PAGE_EXECUTE_WRITECOPY =
      0x00000080,            /**< NtAllocateVirtualMemory logs it as an execute request. */
  X_PAGE_GUARD = 0x00000100, /**< First access raises a guard page exception and clears the flag. */
  X_PAGE_NOCACHE =
      0x00000200, /**< Pages are not cached, which the XDK warns gives poor performance. */
  X_PAGE_WRITECOMBINE =
      0x00000400 /**< Pages are not cached and writes are combined into larger batches. */
};

inline constexpr u32 X_INVALID_SOCKET = ~u32{0};
inline constexpr u32 X_SOCKET_ERROR = static_cast<u32>(-1);

/** Guest layout of an entry of a doubly linked list. */
struct X_LIST_ENTRY {
  be_u32 flink_ptr; /**< Next entry, or the head. */
  be_u32 blink_ptr; /**< Previous entry, or the head. */
};
REX_STATIC_ASSERT_SIZE(X_LIST_ENTRY, 8);

/** Guest layout of an entry of a singly linked list. */
struct X_SINGLE_LIST_ENTRY {
  be_u32 next; /**< Offset 0x0. Points to the next entry. */
};
REX_STATIC_ASSERT_SIZE(X_SINGLE_LIST_ENTRY, 4);

/** Guest layout of the head of an interlocked singly linked list. */
struct X_SLIST_HEADER {
  X_SINGLE_LIST_ENTRY next; /**< Offset 0x0. */
  be_u16 depth;             /**< Offset 0x4. */
  be_u16 sequence;          /**< Offset 0x6. */
};
REX_STATIC_ASSERT_SIZE(X_SLIST_HEADER, 8);

/** Guest layout of a spin lock. */
struct X_KSPINLOCK {
  be_u32 prcb_of_owner; /**< Offset 0x0. Address of the owner's PCR, 0 when free. */
};
REX_STATIC_ASSERT_SIZE(X_KSPINLOCK, 4);

/**
 * Typed guest pointer. Holds a guest address and converts to and from uint32_t.
 *
 * @tparam T The pointee type, for documentation only.
 */
template <typename T>
struct TypedGuestPointer {
  be_u32 value; /**< Guest address. */

  operator u32() const { return value; }
  /**
   * Sets the guest address.
   *
   * @param v The guest address.
   * @return This pointer.
   */
  TypedGuestPointer& operator=(u32 v) {
    value = v;
    return *this;
  }
};

}  // namespace rex
