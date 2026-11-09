/**
 * @file        kernel/types/memory.h
 * @brief       Virtual memory flags and typed guest pointers
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
#include <optional>
#include <type_traits>

#include <rex/types.h>
#include <rex/assert.h>
#include <rex/kernel/memory_view.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/heap.h>

namespace rex::kernel {

/** MEM_* flags used by NtAllocateVirtualMemory. */
enum X_MEM : u32 {
  kXMemCommit = 0x00001000,
  kXMemReserve = 0x00002000,
  kXMemDecommit = 0x00004000, /**< Decommits the pages and keeps the reservation. */
  kXMemRelease = 0x00008000,  /**< Releases the whole reservation. */
  kXMemFree = 0x00010000,     /**< State reported for pages neither reserved nor committed. */
  kXMemPrivate = 0x00020000,  /**< Type NtQueryVirtualMemory reports for every region. */
  kXMemReset = 0x00080000,    /**< Valid only alone. NtAllocateVirtualMemory logs it as a stub. */
  kXMemTopDown = 0x00100000,
  kXMemNoZero = 0x00800000,
  kXMemLargePages = 0x20000000, /**< Backs the allocation with 64 KB pages instead of 4 KB pages. */
  kXMemHeap = 0x40000000,       /**< No code reads it. */
  kXMem16MBPages = 0x80000000   /**< Backs the allocation with 16 MB pages. */
};

/** PAGE_* flags used by NtAllocateVirtualMemory. */
enum X_PAGE : u16 {
  kXPageNoAccess = 0x00000001, /**< Any access faults. */
  kXPageReadOnly = 0x00000002,
  kXPageReadWrite = 0x00000004,
  kXPageWriteCopy = 0x00000008,        /**< No code reads it. */
  kXPageExecute = 0x00000010,          /**< Equivalent to kXPageReadOnly. */
  kXPageExecuteRead = 0x00000020,      /**< Equivalent to kXPageReadOnly. */
  kXPageExecuteReadWrite = 0x00000040, /**< Equivalent to kXPageReadWrite. */
  kXPageExecuteWriteCopy =
      0x00000080,           /**< NtAllocateVirtualMemory logs it as an execute request. */
  kXPageGuard = 0x00000100, /**< First access raises a guard page exception and clears the flag. */
  kXPageNoCache = 0x00000200, /**< Pages are not cached, which the XDK says is slow. */
  kXPageWriteCombine =
      0x00000400 /**< Pages are not cached and writes are combined into larger batches. */
};

}  // namespace rex::kernel

namespace rex::kernel {

namespace detail {

/** True when P is a pointer type or nullptr_t that converts to const T*. */
template <typename P, typename T>
inline constexpr bool kIsHostPointerFor =
    (std::is_pointer_v<P> || std::is_null_pointer_v<P>) && std::is_convertible_v<P, const T*>;

}  // namespace detail

/** Typed guest pointer that resolves to a host pointer. */
template <typename T>
struct be_ptr {
  be_u32 value;

  operator u32() const { return value; }

  /**
   * @return The guest address, 0 for a null pointer.
   */
  u32 GetAddress() const { return value; }

  explicit operator bool() const { return value != 0; }

  /**
   * Resolves the pointer through the guest memory of the calling thread.
   *
   * @return The host pointer, null when the guest address is 0.
   */
  T* GetHost() const {
    if (value == 0) {
      return nullptr;
    }
    auto* thread_state = rex::kernel::ThreadState::Get();
    REX_ASSERT(thread_state != nullptr);
    return rex::memory::GuestPtr<T*>(thread_state->GetAddressSpace()->GetVirtualMembase(), value);
  }

  /**
   * Resolves the pointer through the guest memory of the calling thread only when the title could
   * read it.
   *
   * @return The host pointer, null when the address is 0, is misaligned for T, is in no heap,
   * has an unreadable page, or the calling thread has no runtime.
   */
  T* TryGetHost() const {
    auto* thread_state = rex::kernel::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetAddressSpace() == nullptr) {
      return nullptr;
    }
    return thread_state->GetAddressSpace()->TryTranslateVirtual<T*>(value);
  }

  /**
   * Resolves the pointer through a memory view, for a host thread with no bound runtime.
   *
   * @return The host pointer, null when the guest address is 0.
   */
  T* GetHost(const rex::kernel::MemoryView& view) const {
    return value == 0 ? nullptr : view.TranslateVirtual<T*>(value);
  }

  /**
   * Resolves the pointer through a memory view only when the title could read it, for a host
   * thread with no bound runtime.
   *
   * @return The host pointer, null when the address is 0, is misaligned for T, is in no heap, or
   * has an unreadable page.
   */
  T* TryGetHost(const rex::kernel::MemoryView& view) const {
    return view.TryTranslateVirtual<T*>(value);
  }

  T* operator->() const { return GetHost(); }

  be_ptr& operator=(u32 v) {
    value = v;
    return *this;
  }

  /**
   * Sets the pointer to a host pointer into the guest memory of the calling thread.
   *
   * @tparam P The host pointer type, convertible to const T*, or std::nullptr_t.
   * @param host_pointer The host pointer, null for a null pointer.
   */
  template <typename P>
    requires(detail::kIsHostPointerFor<P, T>)
  be_ptr& operator=(P host_pointer) {
    if constexpr (std::is_null_pointer_v<P>) {
      value = 0;
    } else {
      if (host_pointer == nullptr) {
        value = 0;
        return *this;
      }
      auto* thread_state = rex::kernel::ThreadState::Get();
      REX_ASSERT(thread_state != nullptr);
      value = rex::memory::HostPtrToGuest(thread_state->GetAddressSpace()->GetVirtualMembase(),
                                          reinterpret_cast<const void*>(host_pointer));
    }
    return *this;
  }
};

/**
 * Read only view of a guest std::vector, three guest pointers in the release build layout. The
 * title owns the storage. A vector with an allocator or a debug member is not supported.
 *
 * @tparam T The element type, laid out as the title stores it.
 */
template <typename T>
struct GuestVector {
  be_ptr<T> first; /**< Guest address of the first element. */
  be_ptr<T> last;  /**< Guest address one past the last element. */
  be_ptr<T> limit; /**< Guest address one past the allocated storage. */

  /**
   * @return The number of elements, 0 when the pointers are null or out of order.
   */
  size_t size() const {
    const u32 begin_address = first.GetAddress();
    const u32 end_address = last.GetAddress();
    return end_address > begin_address ? (end_address - begin_address) / sizeof(T) : 0;
  }

  bool empty() const { return size() == 0; }

  /**
   * Resolves the first element through the guest memory of the calling thread.
   *
   * @return The host pointer to the first element, null when the vector has no storage.
   */
  const T* begin() const { return first.GetHost(); }

  /**
   * Resolves one past the last element through the guest memory of the calling thread.
   *
   * @return The host pointer past the last element, null when the vector has no storage.
   */
  const T* end() const { return last.GetHost(); }

  /**
   * Resolves the first element through a memory view, for a host thread with no bound runtime.
   *
   * @return The host pointer to the first element, null when the vector has no storage.
   */
  const T* begin(const rex::kernel::MemoryView& view) const { return first.GetHost(view); }

  /**
   * Resolves one past the last element through a memory view, for a host thread with no bound
   * runtime.
   *
   * @return The host pointer past the last element, null when the vector has no storage.
   */
  const T* end(const rex::kernel::MemoryView& view) const { return last.GetHost(view); }

  /**
   * Returns an element through the guest memory of the calling thread without checking the index.
   *
   * @param index The element index, below size().
   */
  const T& operator[](size_t index) const { return begin()[index]; }

  /** Returns the guest address of an element, 0 when the index is not below size(). */
  u32 GetAddress(size_t index) const {
    return index < size() ? first.GetAddress() + static_cast<u32>(index * sizeof(T)) : 0;
  }

  /**
   * Resolves an element through a memory view only when the title could read all of it, for a host
   * thread with no bound runtime.
   *
   * @return The host pointer to the element, null when the index is not below size(), the element
   * is misaligned, wraps the address space, is in no heap, or has an unreadable page.
   */
  const T* TryAt(size_t index, const rex::kernel::MemoryView& view) const {
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
   * Reads a numeric element through a memory view only when the title could read all of it.
   *
   * @param index The element index.
   * @param view The memory view to read through.
   * @return The element in native byte order, nothing when the index is not below size(), the
   * element wraps the address space, is in no heap, or has an unreadable page.
   */
  std::optional<T> TryGet(size_t index, const rex::kernel::MemoryView& view) const
    requires(rex::kernel::detail::GuestScalar<T>)
  {
    const u32 address = GetAddress(index);
    if (address == 0) {
      return std::nullopt;
    }
    return view.TryLoad<T>(address);
  }

  /**
   * Resolves an element through the guest memory of the calling thread only when the title could
   * read all of it.
   *
   * @return The host pointer to the element, null when the index is not below size(), the element
   * is misaligned, wraps the address space, is in no heap, has an unreadable page, or the calling
   * thread has no runtime.
   */
  const T* TryAt(size_t index) const {
    auto* thread_state = rex::kernel::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetAddressSpace() == nullptr) {
      return nullptr;
    }
    return TryAt(index, rex::kernel::MemoryView(*thread_state->GetAddressSpace()));
  }
};
REX_STATIC_ASSERT_SIZE(GuestVector<u8>, 12);

}  // namespace rex::kernel
