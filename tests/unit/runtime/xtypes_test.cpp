/**
 * @file        tests/unit/runtime/xtypes_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/runtime/xtypes.h>

static_assert(rex::X_STATUS_SUCCESS == 0x00000000u);
static_assert(rex::X_STATUS_ABANDONED_WAIT_0 == 0x00000080u);
static_assert(rex::X_STATUS_USER_APC == 0x000000C0u);
static_assert(rex::X_STATUS_ALERTED == 0x00000101u);
static_assert(rex::X_STATUS_TIMEOUT == 0x00000102u);
static_assert(rex::X_STATUS_PENDING == 0x00000103u);
static_assert(rex::X_STATUS_OBJECT_NAME_EXISTS == 0x40000000u);
static_assert(rex::X_STATUS_TIMER_RESUME_IGNORED == 0x40000025u);
static_assert(rex::X_STATUS_BUFFER_OVERFLOW == 0x80000005u);
static_assert(rex::X_STATUS_NO_MORE_FILES == 0x80000006u);
static_assert(rex::X_STATUS_UNSUCCESSFUL == 0xC0000001u);
static_assert(rex::X_STATUS_NOT_IMPLEMENTED == 0xC0000002u);
static_assert(rex::X_STATUS_INVALID_INFO_CLASS == 0xC0000003u);
static_assert(rex::X_STATUS_INFO_LENGTH_MISMATCH == 0xC0000004u);
static_assert(rex::X_STATUS_ACCESS_VIOLATION == 0xC0000005u);
static_assert(rex::X_STATUS_INVALID_HANDLE == 0xC0000008u);
static_assert(rex::X_STATUS_INVALID_PARAMETER == 0xC000000Du);
static_assert(rex::X_STATUS_NO_SUCH_FILE == 0xC000000Fu);
static_assert(rex::X_STATUS_END_OF_FILE == 0xC0000011u);
static_assert(rex::X_STATUS_NO_MEMORY == 0xC0000017u);
static_assert(rex::X_STATUS_ALREADY_COMMITTED == 0xC0000021u);
static_assert(rex::X_STATUS_ACCESS_DENIED == 0xC0000022u);
static_assert(rex::X_STATUS_BUFFER_TOO_SMALL == 0xC0000023u);
static_assert(rex::X_STATUS_OBJECT_TYPE_MISMATCH == 0xC0000024u);
static_assert(rex::X_STATUS_OBJECT_NAME_INVALID == 0xC0000033u);
static_assert(rex::X_STATUS_OBJECT_NAME_NOT_FOUND == 0xC0000034u);
static_assert(rex::X_STATUS_OBJECT_NAME_COLLISION == 0xC0000035u);
static_assert(rex::X_STATUS_SHARING_VIOLATION == 0xC0000043u);
static_assert(rex::X_STATUS_INVALID_PAGE_PROTECTION == 0xC0000045u);
static_assert(rex::X_STATUS_MUTANT_NOT_OWNED == 0xC0000046u);
static_assert(rex::X_STATUS_THREAD_IS_TERMINATING == 0xC000004Bu);
static_assert(rex::X_STATUS_PROCEDURE_NOT_FOUND == 0xC000007Au);
static_assert(rex::X_STATUS_INSUFFICIENT_RESOURCES == 0xC000009Au);
static_assert(rex::X_STATUS_NOT_SAME_DEVICE == 0xC00000D4u);
static_assert(rex::X_STATUS_MEMORY_NOT_ALLOCATED == 0xC00000A0u);
static_assert(rex::X_STATUS_FILE_IS_A_DIRECTORY == 0xC00000BAu);
static_assert(rex::X_STATUS_NOT_SUPPORTED == 0xC00000BBu);
static_assert(rex::X_STATUS_NOT_A_DIRECTORY == 0xC0000103u);
static_assert(rex::X_STATUS_INVALID_PARAMETER_1 == 0xC00000EFu);
static_assert(rex::X_STATUS_INVALID_PARAMETER_2 == 0xC00000F0u);
static_assert(rex::X_STATUS_INVALID_PARAMETER_3 == 0xC00000F1u);
static_assert(rex::X_STATUS_DLL_NOT_FOUND == 0xC0000135u);
static_assert(rex::X_STATUS_ENTRYPOINT_NOT_FOUND == 0xC0000139u);
static_assert(rex::X_STATUS_SEMAPHORE_LIMIT_EXCEEDED == 0xC000012Bu);
static_assert(rex::X_STATUS_MAPPED_ALIGNMENT == 0xC0000220u);
static_assert(rex::X_STATUS_NOT_FOUND == 0xC0000225u);
static_assert(rex::X_STATUS_DRIVER_ORDINAL_NOT_FOUND == 0xC0000262u);
static_assert(rex::X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND == 0xC0000263u);
static_assert(rex::X_ERROR_SUCCESS == 0x00000000u);
static_assert(rex::X_ERROR_FILE_NOT_FOUND == 0x00000002u);
static_assert(rex::X_ERROR_PATH_NOT_FOUND == 0x00000003u);
static_assert(rex::X_ERROR_ACCESS_DENIED == 0x00000005u);
static_assert(rex::X_ERROR_INVALID_HANDLE == 0x00000006u);
static_assert(rex::X_ERROR_NOT_ENOUGH_MEMORY == 0x00000008u);
static_assert(rex::X_ERROR_NO_MORE_FILES == 0x00000012u);
static_assert(rex::X_ERROR_INVALID_PARAMETER == 0x00000057u);
static_assert(rex::X_ERROR_INSUFFICIENT_BUFFER == 0x0000007Au);
static_assert(rex::X_ERROR_INVALID_NAME == 0x0000007Bu);
static_assert(rex::X_ERROR_BAD_ARGUMENTS == 0x000000A0u);
static_assert(rex::X_ERROR_BUSY == 0x000000AAu);
static_assert(rex::X_ERROR_ALREADY_EXISTS == 0x000000B7u);
static_assert(rex::X_ERROR_IO_INCOMPLETE == 0x000003E4u);
static_assert(rex::X_ERROR_IO_PENDING == 0x000003E5u);
static_assert(rex::X_ERROR_DEVICE_NOT_CONNECTED == 0x0000048Fu);
static_assert(rex::X_ERROR_NOT_FOUND == 0x00000490u);
static_assert(rex::X_ERROR_CANCELLED == 0x000004C7u);
static_assert(rex::X_ERROR_NOT_LOGGED_ON == 0x000004DDu);
static_assert(rex::X_ERROR_NO_SUCH_USER == 0x00000525u);
static_assert(rex::X_ERROR_FUNCTION_FAILED == 0x0000065Bu);
static_assert(rex::X_ERROR_EMPTY == 0x000010D2u);
static_assert(rex::X_E_FALSE == 0x80000000u);
static_assert(rex::X_E_FAIL == 0x80004005u);
static_assert(rex::X_E_SUCCESS == 0x00000000u);
static_assert(rex::X_E_NO_MORE_FILES == 0x80070012u);
static_assert(rex::X_E_INVALIDARG == 0x80070057u);
static_assert(rex::X_E_DEVICE_NOT_CONNECTED == 0x8007048Fu);
static_assert(rex::X_E_NOTFOUND == 0x80070490u);
static_assert(rex::X_E_NO_SUCH_USER == 0x80070525u);
static_assert(rex::kXInvalidHandleValue == 0xFFFFFFFFu);
static_assert(rex::kXInvalidSocket == 0xFFFFFFFFu);
static_assert(rex::kXSocketError == 0xFFFFFFFFu);
static_assert(rex::XHResultFromWin32(0) == 0u);
static_assert(rex::XHResultFromWin32(0x80070057u) == 0x80070057u);
static_assert(rex::XHResultFromWin32(0x00000057u) == 0x80070057u);
static_assert(rex::XResultFromWin32(0x00000490u) == 0x00000490u);

REX_TEST_CASE("XSUCCEEDED accepts codes whose top 2 bits are clear", "[runtime][xtypes]") {
  CHECK(rex::XSUCCEEDED(rex::X_STATUS_SUCCESS));
  CHECK(rex::XSUCCEEDED(rex::X_STATUS_PENDING));
  CHECK_FALSE(rex::XSUCCEEDED(rex::X_STATUS_OBJECT_NAME_EXISTS));
  CHECK_FALSE(rex::XSUCCEEDED(rex::X_STATUS_BUFFER_OVERFLOW));
  CHECK_FALSE(rex::XSUCCEEDED(rex::X_STATUS_UNSUCCESSFUL));
  CHECK(rex::XFAILED(rex::X_STATUS_INVALID_PARAMETER));
  CHECK_FALSE(rex::XFAILED(rex::X_STATUS_TIMEOUT));
}
