/**
 * @file        runtime/xio.h
 * @brief       I/O structures (overlapped, strings, file attributes)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/memory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

#pragma pack(push, 4)

/** File attribute flags. */
enum X_FILE_ATTRIBUTES : u32 {
  X_FILE_ATTRIBUTE_NONE = 0x0000,     /**< No attribute bit is set. */
  X_FILE_ATTRIBUTE_READONLY = 0x0001, /**< The file cannot be written. */
  X_FILE_ATTRIBUTE_HIDDEN =
      0x0002, /**< NT attribute bit the XDK does not list, no code reads it. */
  X_FILE_ATTRIBUTE_SYSTEM =
      0x0004, /**< NT attribute bit the XDK does not list, no code reads it. */
  X_FILE_ATTRIBUTE_DIRECTORY = 0x0010, /**< The entry is a directory. */
  X_FILE_ATTRIBUTE_ARCHIVE = 0x0020,   /**< The entry is an archive file or directory. */
  X_FILE_ATTRIBUTE_DEVICE = 0x0040,    /**< Reserved, titles do not use it. */
  X_FILE_ATTRIBUTE_NORMAL = 0x0080, /**< The file has no other attribute, valid only on its own. */
  /** NT attribute bit the XDK does not list, no code reads it. */
  X_FILE_ATTRIBUTE_TEMPORARY = 0x0100,
  X_FILE_ATTRIBUTE_COMPRESSED =
      0x0800, /**< NT attribute bit the XDK does not list, no code reads it. */
  X_FILE_ATTRIBUTE_ENCRYPTED =
      0x4000, /**< NT attribute bit the XDK does not list, no code reads it. */
};

// Known as XOVERLAPPED to recompiled code.
struct XAM_OVERLAPPED {
  be_u32 result;             /**< Offset 0x0. */
  be_u32 length;             /**< Offset 0x4. */
  be_u32 context;            /**< Offset 0x8. */
  be_u32 event;              /**< Offset 0xC. */
  be_u32 completion_routine; /**< Offset 0x10. */
  be_u32 completion_context; /**< Offset 0x14. */
  be_u32 extended_error;     /**< Offset 0x18. */
};

REX_STATIC_ASSERT_SIZE(XAM_OVERLAPPED, 0x1C);

inline void XOverlappedSetResult(void* ptr, uint32_t value) {
  static_cast<XAM_OVERLAPPED*>(ptr)->result.store(value, std::memory_order_release);
}
inline uint32_t XOverlappedGetResult(void* ptr) {
  return static_cast<XAM_OVERLAPPED*>(ptr)->result.load(std::memory_order_acquire);
}
inline void XOverlappedSetLength(void* ptr, uint32_t value) {
  static_cast<XAM_OVERLAPPED*>(ptr)->length = value;
}
inline uint32_t XOverlappedGetContext(void* ptr) {
  return static_cast<XAM_OVERLAPPED*>(ptr)->context;
}
inline void XOverlappedSetContext(void* ptr, uint32_t value) {
  static_cast<XAM_OVERLAPPED*>(ptr)->context = value;
}
inline X_HANDLE XOverlappedGetEvent(void* ptr) {
  return static_cast<XAM_OVERLAPPED*>(ptr)->event;
}
inline uint32_t XOverlappedGetCompletionRoutine(void* ptr) {
  return static_cast<XAM_OVERLAPPED*>(ptr)->completion_routine;
}
inline void XOverlappedSetExtendedError(void* ptr, uint32_t value) {
  static_cast<XAM_OVERLAPPED*>(ptr)->extended_error = value;
}

struct X_ANSI_STRING {
  be_u16 length;         /**< Offset 0x0. Used bytes of the string, without a terminator. */
  be_u16 maximum_length; /**< Offset 0x2. Capacity in bytes of the buffer at pointer. */
  be_u32 pointer;        /**< Offset 0x4. Guest address of the characters. */

  void reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_ANSI_STRING, 8);

struct X_UNICODE_STRING {
  be_u16 length;         /**< Offset 0x0. */
  be_u16 maximum_length; /**< Offset 0x2. */
  be_u32 pointer;        /**< Offset 0x4. */

  void reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_UNICODE_STRING, 8);

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff550671(v=vs.85).aspx
struct X_IO_STATUS_BLOCK {
  union {
    be<X_STATUS> status; /**< Offset 0x0. Final status of the call. */
    be_u32 pointer;      /**< Offset 0x0. Shares its dword with status. */
  };
  /** Offset 0x4. Call specific result, such as the bytes transferred or the file action. */
  be_u32 information;
};

struct X_OBJECT_ATTRIBUTES {
  be_u32 root_directory; /**< Offset 0x0. */
  be_u32 name_ptr;       /**< Offset 0x4. PANSI_STRING. */
  be_u32 attributes;     /**< Offset 0xC. */
};

#pragma pack(pop)

}  // namespace rex::runtime
