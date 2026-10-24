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

#include <rex/assert.h>
#include <rex/memory.h>
#include <rex/runtime/xstatus.h>

namespace rex::runtime {

#pragma pack(push, 4)

/** File attribute flags. */
enum X_FILE_ATTRIBUTES : u16 {
  kXFileAttributeNone = 0x0000,     /**< No attribute bit is set. */
  kXFileAttributeReadOnly = 0x0001, /**< The file cannot be written. */
  kXFileAttributeHidden = 0x0002, /**< NT attribute bit the XDK does not list, no code reads it. */
  kXFileAttributeSystem = 0x0004, /**< NT attribute bit the XDK does not list, no code reads it. */
  kXFileAttributeDirectory = 0x0010, /**< The entry is a directory. */
  kXFileAttributeArchive = 0x0020,   /**< The entry is an archive file or directory. */
  kXFileAttributeDevice = 0x0040,    /**< Reserved, titles do not use it. */
  kXFileAttributeNormal = 0x0080,    /**< The file has no other attribute, valid only on its own. */
  /** NT attribute bit the XDK does not list, no code reads it. */
  kXFileAttributeTemporary = 0x0100,
  kXFileAttributeCompressed =
      0x0800, /**< NT attribute bit the XDK does not list, no code reads it. */
  kXFileAttributeEncrypted =
      0x4000, /**< NT attribute bit the XDK does not list, no code reads it. */
};

/** The overlapped IO block, known as XOVERLAPPED to the title. */
struct XOVERLAPPED {
  be_u32 result;
  be_u32 length;
  be_u32 context;
  be_u32 event;
  be_u32 completion_routine;
  be_u32 completion_context;
  be_u32 extended_error;
};

REX_STATIC_ASSERT_SIZE(XOVERLAPPED, 0x1C);

/**
 * Publishes the result of an overlapped call with a release store.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @param value The result.
 */
inline void XOverlappedSetResult(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->result.Store(value, std::memory_order_release);
}

/**
 * Returns the result of an overlapped call with an acquire load.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @return The result.
 */
inline u32 XOverlappedGetResult(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->result.Load(std::memory_order_acquire);
}

/**
 * Sets the transferred length of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @param value The length.
 */
inline void XOverlappedSetLength(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->length = value;
}

/**
 * Returns the context of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @return The context.
 */
inline u32 XOverlappedGetContext(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->context;
}

/**
 * Sets the context of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @param value The context.
 */
inline void XOverlappedSetContext(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->context = value;
}

/**
 * Returns the event of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @return The event handle, or 0 when there is none.
 */
inline X_HANDLE XOverlappedGetEvent(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->event;
}

/**
 * Returns the completion routine of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @return The guest address of the routine, or 0 when there is none.
 */
inline u32 XOverlappedGetCompletionRoutine(void* ptr) {
  return static_cast<XOVERLAPPED*>(ptr)->completion_routine;
}

/**
 * Sets the extended error of an overlapped call.
 *
 * @param ptr The XAM_OVERLAPPED block.
 * @param value The extended error.
 */
inline void XOverlappedSetExtendedError(void* ptr, u32 value) {
  static_cast<XOVERLAPPED*>(ptr)->extended_error = value;
}

/** A counted 8 bit string in guest memory. */
struct X_ANSI_STRING {
  be_u16 length;         /**< Used bytes of the string, without a terminator. */
  be_u16 maximum_length; /**< Capacity in bytes of the buffer at pointer. */
  be_u32 pointer;        /**< Guest address of the characters. */

  /** Clears the length, capacity and pointer. */
  void Reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_ANSI_STRING, 8);

/** A counted 16 bit string in guest memory. */
struct X_UNICODE_STRING {
  be_u16 length;
  be_u16 maximum_length;
  be_u32 pointer;

  /** Clears the length, capacity and pointer. */
  void Reset() {
    length = 0;
    maximum_length = 0;
    pointer = 0;
  }
};
REX_STATIC_ASSERT_SIZE(X_UNICODE_STRING, 8);

/** The status and information of a completed IO call. */
struct X_IO_STATUS_BLOCK {
  union {
    BE<X_STATUS> status; /**< Final status of the call. */
    be_u32 pointer;      /**< Shares its dword with status. */
  };
  /** Call specific result, such as the bytes transferred or the file action. */
  be_u32 information;
};

/** The root directory, name and attributes of an object to open. */
struct X_OBJECT_ATTRIBUTES {
  be_u32 root_directory;
  be_u32 name_ptr; /**< PANSI_STRING. */
  be_u32 attributes;
};
REX_STATIC_ASSERT_SIZE(X_OBJECT_ATTRIBUTES, 0xC);

#pragma pack(pop)

}  // namespace rex::runtime
