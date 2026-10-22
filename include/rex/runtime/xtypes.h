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

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <rex/assert.h>
#include <rex/runtime/memory_view.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
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

/** MEM_* flags used by NtAllocateVirtualMemory. */
enum X_MEM : u32 {
  kXMemCommit = 0x00001000,   /**< Commits the pages. */
  kXMemReserve = 0x00002000,  /**< Reserves the address range. */
  kXMemDecommit = 0x00004000, /**< Decommits the pages and keeps the reservation. */
  kXMemRelease = 0x00008000,  /**< Releases the whole reservation. */
  kXMemFree = 0x00010000,     /**< State NtQueryVirtualMemory reports for pages neither reserved nor
                                  committed. */
  kXMemPrivate = 0x00020000,  /**< Type NtQueryVirtualMemory reports for every region. */
  kXMemReset = 0x00080000,    /**< Valid only alone, NtAllocateVirtualMemory logs it as a stub. */
  kXMemTopDown = 0x00100000,  /**< Allocates at the highest free address. */
  kXMemNoZero = 0x00800000,   /**< Leaves the new pages unzeroed. */
  kXMemLargePages = 0x20000000, /**< Backs the allocation with 64 KB pages instead of 4 KB pages. */
  kXMemHeap = 0x40000000,       /**< No code reads it. */
  kXMem16MBPages = 0x80000000   /**< Backs the allocation with 16 MB pages. */
};

/** PAGE_* flags used by NtAllocateVirtualMemory. */
enum X_PAGE : u16 {
  kXPageNoAccess = 0x00000001,         /**< Any access faults. */
  kXPageReadOnly = 0x00000002,         /**< Pages can be read. */
  kXPageReadWrite = 0x00000004,        /**< Pages can be read and written. */
  kXPageWriteCopy = 0x00000008,        /**< No code reads it. */
  kXPageExecute = 0x00000010,          /**< Equivalent to kXPageReadOnly. */
  kXPageExecuteRead = 0x00000020,      /**< Equivalent to kXPageReadOnly. */
  kXPageExecuteReadWrite = 0x00000040, /**< Equivalent to kXPageReadWrite. */
  kXPageExecuteWriteCopy =
      0x00000080,           /**< NtAllocateVirtualMemory logs it as an execute request. */
  kXPageGuard = 0x00000100, /**< First access raises a guard page exception and clears the flag. */
  kXPageNoCache =
      0x00000200, /**< Pages are not cached, which the XDK warns gives poor performance. */
  kXPageWriteCombine =
      0x00000400 /**< Pages are not cached and writes are combined into larger batches. */
};

inline constexpr u32 kXInvalidSocket = ~u32{0};
inline constexpr u32 kXSocketError = static_cast<u32>(-1);

/** Guest layout of an entry of a doubly linked list. */
struct X_LIST_ENTRY {
  be_u32 flink_ptr; /**< Next entry, or the head. */
  be_u32 blink_ptr; /**< Previous entry, or the head. */
};
REX_STATIC_ASSERT_SIZE(X_LIST_ENTRY, 8);

/** Guest layout of an entry of a singly linked list. */
struct X_SINGLE_LIST_ENTRY {
  be_u32 next; /**< Points to the next entry. */
};
REX_STATIC_ASSERT_SIZE(X_SINGLE_LIST_ENTRY, 4);

/** Guest layout of the head of an interlocked singly linked list. */
struct X_SLIST_HEADER {
  X_SINGLE_LIST_ENTRY next;
  be_u16 depth;
  be_u16 sequence;
};
REX_STATIC_ASSERT_SIZE(X_SLIST_HEADER, 8);

/** Guest layout of a spin lock. */
struct X_KSPINLOCK {
  be_u32 owner_pcr; /**< Address of the owner's PCR, 0 when free. */
};
REX_STATIC_ASSERT_SIZE(X_KSPINLOCK, 4);

namespace detail {

/**
 * True when P is a pointer type or nullptr_t that converts to const T*.
 *
 * @tparam P The candidate host pointer type.
 * @tparam T The pointee type.
 */
template <typename P, typename T>
inline constexpr bool kIsHostPointerFor =
    (std::is_pointer_v<P> || std::is_null_pointer_v<P>) && std::is_convertible_v<P, const T*>;

}  // namespace detail

/**
 * Typed guest pointer. Holds a guest address, converts to and from uint32_t and resolves to a
 * host pointer.
 *
 * @tparam T The pointee type.
 */
template <typename T>
struct be_ptr {
  be_u32 value; /**< Guest address. */

  operator u32() const { return value; }

  /**
   * Returns the guest address.
   *
   * @return The guest address, 0 for a null pointer.
   */
  u32 GetAddress() const { return value; }

  /**
   * Reports whether the pointer is not null.
   *
   * @return True when the guest address is not 0.
   */
  explicit operator bool() const { return value != 0; }

  /**
   * Resolves the pointer through the guest memory of the calling thread.
   *
   * @return The host pointer, null when the guest address is 0.
   */
  T* get() const {
    if (value == 0) {
      return nullptr;
    }
    auto* thread_state = rex::runtime::ThreadState::Get();
    REX_ASSERT(thread_state != nullptr);
    return rex::runtime::GuestPtr<T*>(thread_state->GetMemory()->GetVirtualMembase(), value);
  }

  /**
   * Resolves the pointer through the guest memory of the calling thread only when the title could
   * read it.
   *
   * @return The host pointer, null when the address is 0, is misaligned for T, is in no heap,
   * has an unreadable page, or the calling thread has no runtime.
   */
  T* TryGet() const {
    auto* thread_state = rex::runtime::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetMemory() == nullptr) {
      return nullptr;
    }
    return thread_state->GetMemory()->TryTranslateVirtual<T*>(value);
  }

  /**
   * Resolves the pointer through a memory view, for a host thread with no bound runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer, null when the guest address is 0.
   */
  T* get(const rex::runtime::MemoryView& view) const {
    return value == 0 ? nullptr : view.TranslateVirtual<T*>(value);
  }

  /**
   * Resolves the pointer through a memory view only when the title could read it, for a host
   * thread with no bound runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer, null when the address is 0, is misaligned for T, is in no heap, or
   * has an unreadable page.
   */
  T* TryGet(const rex::runtime::MemoryView& view) const {
    return view.TryTranslateVirtual<T*>(value);
  }

  /**
   * Resolves the pointer through the guest memory of the calling thread.
   *
   * @return The host pointer.
   */
  T* operator->() const { return get(); }

  /**
   * Sets the guest address.
   *
   * @param v The guest address.
   * @return This pointer.
   */
  be_ptr& operator=(u32 v) {
    value = v;
    return *this;
  }

  /**
   * Sets the pointer to a host pointer into the guest memory of the calling thread.
   *
   * @tparam P The host pointer type, convertible to const T*, or std::nullptr_t.
   * @param host_pointer The host pointer, null for a null pointer.
   * @return This pointer.
   */
  template <typename P>
    requires(detail::kIsHostPointerFor<P, T>)
  be_ptr& operator=(P host_pointer) {
    if constexpr (std::is_null_pointer_v<P>) {
      value = 0;
    } else if (host_pointer == nullptr) {
      value = 0;
    } else {
      auto* thread_state = rex::runtime::ThreadState::Get();
      REX_ASSERT(thread_state != nullptr);
      value = rex::runtime::HostPtrToGuest(thread_state->GetMemory()->GetVirtualMembase(),
                                           reinterpret_cast<const void*>(host_pointer));
    }
    return *this;
  }
};

/**
 * Read only view of a guest std::vector, three guest pointers to its storage. The title owns the
 * storage, so the view never grows, shrinks or allocates it. It describes the three pointer layout
 * of a release build. A vector that carries an allocator or a debug member has another layout and
 * is not supported.
 *
 * @tparam T The element type, laid out as the title stores it.
 */
template <typename T>
struct be_vector {
  be_ptr<T> first; /**< Guest address of the first element. */
  be_ptr<T> last;  /**< Guest address one past the last element. */
  be_ptr<T> limit; /**< Guest address one past the allocated storage. */

  /**
   * Returns the element count.
   *
   * @return The number of elements, 0 when the pointers are null or out of order.
   */
  size_t size() const {
    const u32 begin_address = first.GetAddress();
    const u32 end_address = last.GetAddress();
    return end_address > begin_address ? (end_address - begin_address) / sizeof(T) : 0;
  }

  /**
   * Reports whether the vector has no elements.
   *
   * @return True when size() is 0.
   */
  bool empty() const { return size() == 0; }

  /**
   * Resolves the first element through the guest memory of the calling thread.
   *
   * @return The host pointer to the first element, null when the vector has no storage.
   */
  const T* begin() const { return first.get(); }

  /**
   * Resolves one past the last element through the guest memory of the calling thread.
   *
   * @return The host pointer past the last element, null when the vector has no storage.
   */
  const T* end() const { return last.get(); }

  /**
   * Resolves the first element through a memory view, for a host thread with no bound runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer to the first element, null when the vector has no storage.
   */
  const T* begin(const rex::runtime::MemoryView& view) const { return first.get(view); }

  /**
   * Resolves one past the last element through a memory view, for a host thread with no bound
   * runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer past the last element, null when the vector has no storage.
   */
  const T* end(const rex::runtime::MemoryView& view) const { return last.get(view); }

  /**
   * Returns an element through the guest memory of the calling thread without checking the index.
   *
   * @param index The element index, below size().
   * @return The element.
   */
  const T& operator[](size_t index) const { return begin()[index]; }

  /**
   * Returns the guest address of an element.
   *
   * @param index The element index.
   * @return The guest address, 0 when the index is not below size().
   */
  u32 GetAddress(size_t index) const {
    return index < size() ? first.GetAddress() + static_cast<u32>(index * sizeof(T)) : 0;
  }

  /**
   * Resolves an element through a memory view only when the title could read all of it, for a host
   * thread with no bound runtime.
   *
   * @param index The element index.
   * @param view The memory view that holds the guest memory.
   * @return The host pointer to the element, null when the index is not below size(), the element
   * is misaligned, wraps the address space, is in no heap, or has an unreadable page.
   */
  const T* TryAt(size_t index, const rex::runtime::MemoryView& view) const {
    const u32 address = GetAddress(index);
    const u64 last_byte = static_cast<u64>(address) + sizeof(T) - 1;
    if (address == 0 || last_byte > 0xFFFFFFFFull) {
      return nullptr;
    }
    const T* element = view.TryTranslateVirtual<const T*>(address);
    if (element == nullptr ||
        view.TryTranslateVirtual<const u8*>(static_cast<u32>(last_byte)) == nullptr) {
      return nullptr;
    }
    return element;
  }

  /**
   * Resolves an element through the guest memory of the calling thread only when the title could
   * read all of it.
   *
   * @param index The element index.
   * @return The host pointer to the element, null when the index is not below size(), the element
   * is misaligned, wraps the address space, is in no heap, has an unreadable page, or the calling
   * thread has no runtime.
   */
  const T* TryAt(size_t index) const {
    auto* thread_state = rex::runtime::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetMemory() == nullptr) {
      return nullptr;
    }
    return TryAt(index, rex::runtime::MemoryView(*thread_state->GetMemory()));
  }
};
REX_STATIC_ASSERT_SIZE(be_vector<u8>, 12);

}  // namespace rex
