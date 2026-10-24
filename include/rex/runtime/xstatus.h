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
inline constexpr X_HANDLE kXInvalidHandleValue =
    static_cast<X_HANDLE>(-1); /**< Handle value that names no object. */

/** NT status code type, holding STATUS_* values. */
using X_STATUS = u32;
/**
 * Reports whether a status code is a success or informational code.
 *
 * @param s Status code to test.
 * @return True when the severity bits of the status are success or informational.
 */
[[nodiscard]] constexpr bool XSUCCEEDED(X_STATUS s) {
  return (s & 0xC0000000) == 0;
}
/**
 * Reports whether a status code is a warning or error code.
 *
 * @param s Status code to test.
 * @return True when the severity bits of the status are warning or error.
 */
[[nodiscard]] constexpr bool XFAILED(X_STATUS s) {
  return !XSUCCEEDED(s);
}
inline constexpr X_STATUS X_STATUS_SUCCESS =
    0x00000000; /**< The operation completed successfully. */
inline constexpr X_STATUS X_STATUS_ABANDONED_WAIT_0 =
    0x00000080; /**< A wait was satisfied by a mutant abandoned by its owning thread. */
inline constexpr X_STATUS X_STATUS_USER_APC =
    0x000000C0; /**< A user APC delivery interrupted the wait. */
inline constexpr X_STATUS X_STATUS_ALERTED =
    0x00000101; /**< The wait was interrupted by an alert. */
inline constexpr X_STATUS X_STATUS_TIMEOUT =
    0x00000102; /**< The wait timed out before the object was signaled. */
inline constexpr X_STATUS X_STATUS_PENDING = 0x00000103; /**< The operation is still in progress. */
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_EXISTS =
    0x40000000; /**< An object with the requested name already existed and was opened. */
inline constexpr X_STATUS X_STATUS_TIMER_RESUME_IGNORED =
    0x40000025; /**< The resume flag on the timer was ignored. */
inline constexpr X_STATUS X_STATUS_BUFFER_OVERFLOW =
    0x80000005; /**< The buffer was too small and the data was truncated. */
inline constexpr X_STATUS X_STATUS_NO_MORE_FILES =
    0x80000006; /**< The directory enumeration has no more entries. */
inline constexpr X_STATUS X_STATUS_UNSUCCESSFUL =
    0xC0000001; /**< The operation failed without a more specific code. */
inline constexpr X_STATUS X_STATUS_NOT_IMPLEMENTED =
    0xC0000002; /**< The requested operation is not implemented. */
inline constexpr X_STATUS X_STATUS_INVALID_INFO_CLASS =
    0xC0000003; /**< The information class is not valid for the request. */
inline constexpr X_STATUS X_STATUS_INFO_LENGTH_MISMATCH =
    0xC0000004; /**< The supplied information length does not match the information class. */
inline constexpr X_STATUS X_STATUS_ACCESS_VIOLATION =
    0xC0000005; /**< The operation referenced an invalid memory address. */
inline constexpr X_STATUS X_STATUS_INVALID_HANDLE = 0xC0000008;    /**< The handle is not valid. */
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER = 0xC000000D; /**< A parameter is not valid. */
inline constexpr X_STATUS X_STATUS_NO_SUCH_FILE = 0xC000000F;      /**< The file does not exist. */
inline constexpr X_STATUS X_STATUS_END_OF_FILE =
    0xC0000011; /**< A read reached the end of the file. */
inline constexpr X_STATUS X_STATUS_NO_MEMORY =
    0xC0000017; /**< Not enough memory was available to complete the request. */
inline constexpr X_STATUS X_STATUS_ALREADY_COMMITTED =
    0xC0000021; /**< The memory range is already committed. */
inline constexpr X_STATUS X_STATUS_ACCESS_DENIED =
    0xC0000022; /**< The caller lacks the access the operation requires. */
inline constexpr X_STATUS X_STATUS_BUFFER_TOO_SMALL =
    0xC0000023; /**< The supplied buffer is too small for the result. */
inline constexpr X_STATUS X_STATUS_OBJECT_TYPE_MISMATCH =
    0xC0000024; /**< The object is not of the type the operation requires. */
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_INVALID =
    0xC0000033; /**< The object name is not valid. */
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_NOT_FOUND =
    0xC0000034; /**< No object with the requested name exists. */
inline constexpr X_STATUS X_STATUS_OBJECT_NAME_COLLISION =
    0xC0000035; /**< An object with the requested name already exists. */
inline constexpr X_STATUS X_STATUS_SHARING_VIOLATION =
    0xC0000043; /**< The file is in use and its sharing mode does not allow the access. */
inline constexpr X_STATUS X_STATUS_INVALID_PAGE_PROTECTION =
    0xC0000045; /**< The page protection value is not valid. */
inline constexpr X_STATUS X_STATUS_MUTANT_NOT_OWNED =
    0xC0000046; /**< The mutant is not owned by the calling thread. */
inline constexpr X_STATUS X_STATUS_THREAD_IS_TERMINATING =
    0xC000004B; /**< The thread is terminating. */
inline constexpr X_STATUS X_STATUS_PROCESS_IS_TERMINATING =
    0xC000010A; /**< The process is terminating. */
inline constexpr X_STATUS X_STATUS_PROCEDURE_NOT_FOUND =
    0xC000007A; /**< The named procedure was not found. */
inline constexpr X_STATUS X_STATUS_INSUFFICIENT_RESOURCES =
    0xC000009A; /**< Not enough system resources were available to complete the request. */
inline constexpr X_STATUS X_STATUS_NOT_SAME_DEVICE =
    0xC00000D4; /**< The operation cannot span two different devices. */
inline constexpr X_STATUS X_STATUS_MEMORY_NOT_ALLOCATED =
    0xC00000A0; /**< The memory range was not allocated. */
inline constexpr X_STATUS X_STATUS_FILE_IS_A_DIRECTORY =
    0xC00000BA; /**< The path names a directory where a file was expected. */
inline constexpr X_STATUS X_STATUS_NOT_SUPPORTED = 0xC00000BB; /**< The request is not supported. */
inline constexpr X_STATUS X_STATUS_NOT_A_DIRECTORY =
    0xC0000103; /**< The path names something other than a directory where a directory was expected.
                 */
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_1 =
    0xC00000EF; /**< The first parameter is not valid. */
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_2 =
    0xC00000F0; /**< The second parameter is not valid. */
inline constexpr X_STATUS X_STATUS_INVALID_PARAMETER_3 =
    0xC00000F1; /**< The third parameter is not valid. */
inline constexpr X_STATUS X_STATUS_DLL_NOT_FOUND =
    0xC0000135; /**< The named library could not be found. */
inline constexpr X_STATUS X_STATUS_ENTRYPOINT_NOT_FOUND =
    0xC0000139; /**< The named entry point was not found in the library. */
inline constexpr X_STATUS X_STATUS_SEMAPHORE_LIMIT_EXCEEDED =
    0xC000012B; /**< A release would exceed the semaphore maximum count. */
inline constexpr X_STATUS X_STATUS_MAPPED_ALIGNMENT =
    0xC0000220; /**< The base address or file offset is not aligned to the allocation granularity.
                 */
inline constexpr X_STATUS X_STATUS_NOT_FOUND = 0xC0000225; /**< The requested item was not found. */
inline constexpr X_STATUS X_STATUS_DRIVER_ORDINAL_NOT_FOUND =
    0xC0000262; /**< The driver ordinal was not found in the driver. */
inline constexpr X_STATUS X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND =
    0xC0000263; /**< The named driver entry point was not found. */

/** Win32 error code type, holding ERROR_* values. */
using X_RESULT = u32;
inline constexpr i32 kFacilityWin32 = 0x0007; /**< HRESULT facility code for Win32 errors. */
/**
 * Returns the Win32 error code unchanged.
 *
 * @param x Win32 error code.
 * @return The same error code.
 */
[[nodiscard]] constexpr X_RESULT XResultFromWin32(u32 x) {
  return x;
}

inline constexpr X_RESULT X_ERROR_SUCCESS =
    XResultFromWin32(0x00000000); /**< The operation completed successfully. */
inline constexpr X_RESULT X_ERROR_FILE_NOT_FOUND =
    XResultFromWin32(0x00000002); /**< The system cannot find the file specified. */
inline constexpr X_RESULT X_ERROR_PATH_NOT_FOUND =
    XResultFromWin32(0x00000003); /**< The system cannot find the path specified. */
inline constexpr X_RESULT X_ERROR_ACCESS_DENIED =
    XResultFromWin32(0x00000005); /**< Access is denied. */
inline constexpr X_RESULT X_ERROR_INVALID_HANDLE =
    XResultFromWin32(0x00000006); /**< The handle is invalid. */
inline constexpr X_RESULT X_ERROR_NOT_ENOUGH_MEMORY =
    XResultFromWin32(0x00000008); /**< Not enough memory is available to process the command. */
inline constexpr X_RESULT X_ERROR_NO_MORE_FILES =
    XResultFromWin32(0x00000012); /**< There are no more files. */
inline constexpr X_RESULT X_ERROR_INVALID_PARAMETER =
    XResultFromWin32(0x00000057); /**< A parameter is incorrect. */
inline constexpr X_RESULT X_ERROR_INSUFFICIENT_BUFFER =
    XResultFromWin32(0x0000007A); /**< The data area passed to the call is too small. */
inline constexpr X_RESULT X_ERROR_INVALID_NAME = XResultFromWin32(
    0x0000007B); /**< The file name, directory name or volume label syntax is incorrect. */
inline constexpr X_RESULT X_ERROR_BAD_ARGUMENTS =
    XResultFromWin32(0x000000A0); /**< The arguments are not valid. */
inline constexpr X_RESULT X_ERROR_BUSY =
    XResultFromWin32(0x000000AA); /**< The requested resource is in use. */
inline constexpr X_RESULT X_ERROR_ALREADY_EXISTS =
    XResultFromWin32(0x000000B7); /**< Cannot create a file when that file already exists. */
inline constexpr X_RESULT X_ERROR_IO_INCOMPLETE =
    XResultFromWin32(0x000003E4); /**< The overlapped I/O event is not in the signaled state. */
inline constexpr X_RESULT X_ERROR_IO_PENDING =
    XResultFromWin32(0x000003E5); /**< The overlapped I/O operation is in progress. */
inline constexpr X_RESULT X_ERROR_DEVICE_NOT_CONNECTED =
    XResultFromWin32(0x0000048F); /**< The device is not connected. */
inline constexpr X_RESULT X_ERROR_NOT_FOUND =
    XResultFromWin32(0x00000490); /**< The element was not found. */
inline constexpr X_RESULT X_ERROR_CANCELLED =
    XResultFromWin32(0x000004C7); /**< The operation was canceled by the user. */
inline constexpr X_RESULT X_ERROR_NOT_LOGGED_ON =
    XResultFromWin32(0x000004DD); /**< The user is not signed in. */
inline constexpr X_RESULT X_ERROR_NO_SUCH_USER =
    XResultFromWin32(0x00000525); /**< The specified user does not exist. */
inline constexpr X_RESULT X_ERROR_FUNCTION_FAILED =
    XResultFromWin32(0x0000065B); /**< The function failed. */
inline constexpr X_RESULT X_ERROR_EMPTY =
    XResultFromWin32(0x000010D2); /**< The list or collection is empty. */

/** HRESULT code type. */
using X_HRESULT = u32;
/**
 * Returns the HRESULT for a Win32 error code.
 *
 * @param x Win32 error code.
 * @return The HRESULT with the Win32 facility and failure bit set, or the value unchanged when it
 *         is zero or already negative.
 */
[[nodiscard]] constexpr X_HRESULT XHResultFromWin32(u32 x) {
  return static_cast<i32>(x) <= 0 ? x : ((x & 0xFFFF) | (kFacilityWin32 << 16) | 0x80000000);
}

inline constexpr X_HRESULT X_E_FALSE = 0x80000000; /**< Success code meaning false (S_FALSE). */
inline constexpr X_HRESULT X_E_SUCCESS =
    XHResultFromWin32(X_ERROR_SUCCESS);           /**< The operation completed successfully. */
inline constexpr X_HRESULT X_E_FAIL = 0x80004005; /**< Unspecified failure. */
inline constexpr X_HRESULT X_E_NO_MORE_FILES =
    XHResultFromWin32(X_ERROR_NO_MORE_FILES); /**< There are no more files. */
inline constexpr X_HRESULT X_E_INVALIDARG =
    XHResultFromWin32(X_ERROR_INVALID_PARAMETER); /**< An argument is not valid. */
inline constexpr X_HRESULT X_E_DEVICE_NOT_CONNECTED =
    XHResultFromWin32(X_ERROR_DEVICE_NOT_CONNECTED); /**< The device is not connected. */
inline constexpr X_HRESULT X_E_NOTFOUND =
    XHResultFromWin32(X_ERROR_NOT_FOUND); /**< The element was not found. */
inline constexpr X_HRESULT X_E_NO_SUCH_USER =
    XHResultFromWin32(X_ERROR_NO_SUCH_USER); /**< The specified user does not exist. */

}  // namespace rex
