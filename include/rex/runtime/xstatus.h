/**
 * @file        runtime/xstatus.h
 * @brief       Status, result and handle types (X_STATUS, X_RESULT, X_HRESULT, X_HANDLE)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex {

/** Value the title uses to name a kernel object. */
using X_HANDLE = u32;
inline constexpr X_HANDLE kXInvalidHandleValue = static_cast<X_HANDLE>(-1);

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
inline constexpr i32 kFacilityWin32 = 0x0007;
[[nodiscard]] constexpr X_RESULT XResultFromWin32(u32 x) {
  return x;
}

inline constexpr X_RESULT X_ERROR_SUCCESS = XResultFromWin32(0x00000000);
inline constexpr X_RESULT X_ERROR_FILE_NOT_FOUND = XResultFromWin32(0x00000002);
inline constexpr X_RESULT X_ERROR_PATH_NOT_FOUND = XResultFromWin32(0x00000003);
inline constexpr X_RESULT X_ERROR_ACCESS_DENIED = XResultFromWin32(0x00000005);
inline constexpr X_RESULT X_ERROR_INVALID_HANDLE = XResultFromWin32(0x00000006);
inline constexpr X_RESULT X_ERROR_NOT_ENOUGH_MEMORY = XResultFromWin32(0x00000008);
inline constexpr X_RESULT X_ERROR_NO_MORE_FILES = XResultFromWin32(0x00000012);
inline constexpr X_RESULT X_ERROR_INVALID_PARAMETER = XResultFromWin32(0x00000057);
inline constexpr X_RESULT X_ERROR_INSUFFICIENT_BUFFER = XResultFromWin32(0x0000007A);
inline constexpr X_RESULT X_ERROR_INVALID_NAME = XResultFromWin32(0x0000007B);
inline constexpr X_RESULT X_ERROR_BAD_ARGUMENTS = XResultFromWin32(0x000000A0);
inline constexpr X_RESULT X_ERROR_BUSY = XResultFromWin32(0x000000AA);
inline constexpr X_RESULT X_ERROR_ALREADY_EXISTS = XResultFromWin32(0x000000B7);
inline constexpr X_RESULT X_ERROR_IO_INCOMPLETE = XResultFromWin32(0x000003E4);
inline constexpr X_RESULT X_ERROR_IO_PENDING = XResultFromWin32(0x000003E5);
inline constexpr X_RESULT X_ERROR_DEVICE_NOT_CONNECTED = XResultFromWin32(0x0000048F);
inline constexpr X_RESULT X_ERROR_NOT_FOUND = XResultFromWin32(0x00000490);
inline constexpr X_RESULT X_ERROR_CANCELLED = XResultFromWin32(0x000004C7);
inline constexpr X_RESULT X_ERROR_NOT_LOGGED_ON = XResultFromWin32(0x000004DD);
inline constexpr X_RESULT X_ERROR_NO_SUCH_USER = XResultFromWin32(0x00000525);
inline constexpr X_RESULT X_ERROR_FUNCTION_FAILED = XResultFromWin32(0x0000065B);
inline constexpr X_RESULT X_ERROR_EMPTY = XResultFromWin32(0x000010D2);

/** HRESULT code type. */
using X_HRESULT = u32;
[[nodiscard]] constexpr X_HRESULT XHResultFromWin32(u32 x) {
  return static_cast<i32>(x) <= 0 ? x : ((x & 0xFFFF) | (kFacilityWin32 << 16) | 0x80000000);
}

inline constexpr X_HRESULT X_E_FALSE = 0x80000000;
inline constexpr X_HRESULT X_E_SUCCESS = XHResultFromWin32(X_ERROR_SUCCESS);
inline constexpr X_HRESULT X_E_FAIL = 0x80004005;
inline constexpr X_HRESULT X_E_NO_MORE_FILES = XHResultFromWin32(X_ERROR_NO_MORE_FILES);
inline constexpr X_HRESULT X_E_INVALIDARG = XHResultFromWin32(X_ERROR_INVALID_PARAMETER);
inline constexpr X_HRESULT X_E_DEVICE_NOT_CONNECTED =
    XHResultFromWin32(X_ERROR_DEVICE_NOT_CONNECTED);
inline constexpr X_HRESULT X_E_NOTFOUND = XHResultFromWin32(X_ERROR_NOT_FOUND);
inline constexpr X_HRESULT X_E_NO_SUCH_USER = XHResultFromWin32(X_ERROR_NO_SUCH_USER);

}  // namespace rex
