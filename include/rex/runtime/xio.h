/**
 * @file        kernel/xio.h
 * @brief       I/O structures (overlapped, strings, file attributes)
 *
 * @copyright   Copyright 2020 Ben Vanik. All rights reserved. (Xenia Project)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/memory.h>
#include <rex/runtime/xtypes.h>

namespace rex::runtime {

#pragma pack(push, 4)

enum X_FILE_ATTRIBUTES : uint32_t {
  X_FILE_ATTRIBUTE_NONE = 0x0000,
  X_FILE_ATTRIBUTE_READONLY = 0x0001,
  X_FILE_ATTRIBUTE_HIDDEN = 0x0002,
  X_FILE_ATTRIBUTE_SYSTEM = 0x0004,
  X_FILE_ATTRIBUTE_DIRECTORY = 0x0010,
  X_FILE_ATTRIBUTE_ARCHIVE = 0x0020,
  X_FILE_ATTRIBUTE_DEVICE = 0x0040,
  X_FILE_ATTRIBUTE_NORMAL = 0x0080,
  X_FILE_ATTRIBUTE_TEMPORARY = 0x0100,
  X_FILE_ATTRIBUTE_COMPRESSED = 0x0800,
  X_FILE_ATTRIBUTE_ENCRYPTED = 0x4000,
};

// Known as XOVERLAPPED to recompiled code.
struct XAM_OVERLAPPED {
  be_u32 result;              // 0x0
  be_u32 length;              // 0x4
  be_u32 context;             // 0x8
  be_u32 event;               // 0xC
  be_u32 completion_routine;  // 0x10
  be_u32 completion_context;  // 0x14
  be_u32 extended_error;      // 0x18
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
  be_u16 length;
  be_u16 maximum_length;
  be_u32 pointer;

  void reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_ANSI_STRING, 8);

struct X_UNICODE_STRING {
  be_u16 length;          // 0x0
  be_u16 maximum_length;  // 0x2
  be_u32 pointer;         // 0x4

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
    be<X_STATUS> status;
    be_u32 pointer;
  };
  be_u32 information;
};

struct X_OBJECT_ATTRIBUTES {
  be_u32 root_directory;  // 0x0
  be_u32 name_ptr;        // 0x4 PANSI_STRING
  be_u32 attributes;      // 0xC
};

#pragma pack(pop)

}  // namespace rex::runtime
