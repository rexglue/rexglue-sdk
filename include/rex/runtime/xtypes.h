/**
 * @file        kernel/xtypes.h
 * @brief       Core type definitions (X_STATUS, X_RESULT, X_HRESULT)
 *
 * @copyright   Copyright 2020 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>

#include <rex/assert.h>
#include <rex/types.h>  // be<T>

namespace rex {

using X_HANDLE = uint32_t;
inline constexpr X_HANDLE X_INVALID_HANDLE_VALUE = static_cast<X_HANDLE>(-1);

// NT_STATUS (STATUS_*)
// https://msdn.microsoft.com/en-us/library/cc704588.aspx
using X_STATUS = uint32_t;
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

// Win32 error codes (ERROR_*)
// https://msdn.microsoft.com/en-us/library/windows/desktop/ms681381(v=vs.85).aspx
using X_RESULT = uint32_t;
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

// HRESULT codes
using X_HRESULT = uint32_t;
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

// MEM_*, used by NtAllocateVirtualMemory
enum X_MEM : uint32_t {
  X_MEM_COMMIT = 0x00001000,
  X_MEM_RESERVE = 0x00002000,
  X_MEM_DECOMMIT = 0x00004000,
  X_MEM_RELEASE = 0x00008000,
  X_MEM_FREE = 0x00010000,
  X_MEM_PRIVATE = 0x00020000,
  X_MEM_RESET = 0x00080000,
  X_MEM_TOP_DOWN = 0x00100000,
  X_MEM_NOZERO = 0x00800000,
  X_MEM_LARGE_PAGES = 0x20000000,
  X_MEM_HEAP = 0x40000000,
  X_MEM_16MB_PAGES = 0x80000000  // from Valve SDK
};

// PAGE_*, used by NtAllocateVirtualMemory
enum X_PAGE : uint32_t {
  X_PAGE_NOACCESS = 0x00000001,
  X_PAGE_READONLY = 0x00000002,
  X_PAGE_READWRITE = 0x00000004,
  X_PAGE_WRITECOPY = 0x00000008,
  X_PAGE_EXECUTE = 0x00000010,
  X_PAGE_EXECUTE_READ = 0x00000020,
  X_PAGE_EXECUTE_READWRITE = 0x00000040,
  X_PAGE_EXECUTE_WRITECOPY = 0x00000080,
  X_PAGE_GUARD = 0x00000100,
  X_PAGE_NOCACHE = 0x00000200,
  X_PAGE_WRITECOMBINE = 0x00000400
};

// Sockets/networking
inline constexpr u32 X_INVALID_SOCKET = ~u32{0};
inline constexpr u32 X_SOCKET_ERROR = static_cast<u32>(-1);

// https://docs.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-list_entry
struct X_LIST_ENTRY {
  be_u32 flink_ptr;  // next entry / head
  be_u32 blink_ptr;  // previous entry / head
};
REX_STATIC_ASSERT_SIZE(X_LIST_ENTRY, 8);

struct X_SINGLE_LIST_ENTRY {
  be_u32 next;  // 0x0 pointer to next entry
};
REX_STATIC_ASSERT_SIZE(X_SINGLE_LIST_ENTRY, 4);

// https://www.nirsoft.net/kernel_struct/vista/SLIST_HEADER.html
struct X_SLIST_HEADER {
  X_SINGLE_LIST_ENTRY next;  // 0x0
  be_u16 depth;              // 0x4
  be_u16 sequence;           // 0x6
};
REX_STATIC_ASSERT_SIZE(X_SLIST_HEADER, 8);

struct X_KSPINLOCK {
  be_u32 prcb_of_owner;
};
REX_STATIC_ASSERT_SIZE(X_KSPINLOCK, 4);

// Typed guest pointer - holds a guest address but provides type documentation.
// Implicitly converts to/from uint32_t via the be<uint32_t> member.
template <typename T>
struct TypedGuestPointer {
  be_u32 value;

  operator uint32_t() const { return value; }
  TypedGuestPointer& operator=(uint32_t v) {
    value = v;
    return *this;
  }
};

}  // namespace rex
