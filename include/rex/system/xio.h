/**
 * @file        kernel/xio.h
 * @brief       Xbox 360 I/O structures (overlapped, strings, file attributes)
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
#include <rex/system/xtypes.h>

namespace rex::system {

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

// Known as XOVERLAPPED to 360 code.
struct XAM_OVERLAPPED {
  be<uint32_t> result;              // 0x0
  be<uint32_t> length;              // 0x4
  be<uint32_t> context;             // 0x8
  be<uint32_t> event;               // 0xC
  be<uint32_t> completion_routine;  // 0x10
  be<uint32_t> completion_context;  // 0x14
  be<uint32_t> extended_error;      // 0x18
};

inline uint32_t XOverlappedGetResult(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[0]);
}
inline void XOverlappedSetResult(void* ptr, uint32_t value) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  memory::store_and_swap<uint32_t>(&p[0], value);
}
inline uint32_t XOverlappedGetLength(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[1]);
}
inline void XOverlappedSetLength(void* ptr, uint32_t value) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  memory::store_and_swap<uint32_t>(&p[1], value);
}
inline uint32_t XOverlappedGetContext(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[2]);
}
inline void XOverlappedSetContext(void* ptr, uint32_t value) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  memory::store_and_swap<uint32_t>(&p[2], value);
}
inline X_HANDLE XOverlappedGetEvent(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[3]);
}
inline uint32_t XOverlappedGetCompletionRoutine(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[4]);
}
inline uint32_t XOverlappedGetCompletionContext(void* ptr) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  return memory::load_and_swap<uint32_t>(&p[5]);
}
inline void XOverlappedSetExtendedError(void* ptr, uint32_t value) {
  auto p = reinterpret_cast<uint32_t*>(ptr);
  memory::store_and_swap<uint32_t>(&p[6], value);
}

struct X_ANSI_STRING {
  be<uint16_t> length;
  be<uint16_t> maximum_length;
  be<uint32_t> pointer;

  void reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
static_assert_size(X_ANSI_STRING, 8);

struct X_UNICODE_STRING {
  be<uint16_t> length;          // 0x0
  be<uint16_t> maximum_length;  // 0x2
  be<uint32_t> pointer;         // 0x4

  void reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
static_assert_size(X_UNICODE_STRING, 8);

// https://msdn.microsoft.com/en-us/library/windows/hardware/ff550671(v=vs.85).aspx
struct X_IO_STATUS_BLOCK {
  union {
    be<X_STATUS> status;
    be<uint32_t> pointer;
  };
  be<uint32_t> information;
};

struct X_OBJECT_ATTRIBUTES {
  be<uint32_t> root_directory;  // 0x0
  be<uint32_t> name_ptr;        // 0x4 PANSI_STRING
  be<uint32_t> attributes;      // 0xC
};

#pragma pack(pop)

}  // namespace rex::system
