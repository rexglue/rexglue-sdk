/**
 * @file        kernel/guest_memory.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::kernel {

class TitleState;
class GuestAllocator;
class Memory;
class MemoryView;

enum class GuestProtect : u8 {
  kReadOnly,
  kReadWrite,
};

enum class GuestHeap : u8 {
  kTitle,    /**< Title memory, which counts against the title budget. */
  kSystem,   /**< System memory, kept apart from title memory and zeroed. */
  kPhysical, /**< Physical memory in 64 KiB pages, which counts against the title budget. */
};

/**
 * Checked guest virtual memory, used by everything except recompiled code. A call whose range is
 * not mapped with the needed access returns a kMemory error and touches nothing. Spans stay valid
 * until TitleRuntime::Unload.
 */
class REX_KERNEL_API GuestMemory {
 public:
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  /**
   * Copies guest memory into a buffer.
   *
   * @param out The buffer to fill. Its size is the length.
   * @return Ok, or a kMemory error.
   */
  [[nodiscard]] Result<void> CopyFrom(u32 address, std::span<std::byte> out) const;

  /**
   * Copies bytes into guest memory.
   *
   * @param data The bytes to copy.
   * @return Ok, or a kMemory error when any byte is not mapped writable.
   */
  [[nodiscard]] Result<void> CopyTo(u32 address, std::span<const std::byte> data);

  /** Returns guest memory to read in place, or a kMemory error. */
  [[nodiscard]] Result<std::span<const std::byte>> GetReadOnlySpan(u32 address, u32 length) const;

  /**
   * Returns guest memory to write in place, or a kMemory error when any byte is not mapped
   * writable.
   */
  [[nodiscard]] Result<std::span<std::byte>> GetWritableSpan(u32 address, u32 length);

  /**
   * Reads a guest value.
   *
   * @tparam T A trivially copyable guest type, such as be_u32 or a struct of be_ fields.
   * @return A copy, or a kMemory error.
   */
  template <typename T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] Result<T> Load(u32 address) const;

  /**
   * Writes a guest value.
   *
   * @tparam T A trivially copyable guest type, such as be_u32 or a struct of be_ fields.
   * @return Ok, or a kMemory error.
   */
  template <typename T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] Result<void> Store(u32 address, const T& value);

  /**
   * Reads a zero terminated 8 bit string.
   *
   * @param max_length The character limit when no terminator comes first.
   * @return The string, or a kMemory error.
   */
  [[nodiscard]] Result<std::string> ReadString(u32 address, u32 max_length = 4096) const;

  /**
   * Reads a zero terminated big endian UTF-16 string.
   *
   * @param max_length The character limit when no terminator comes first.
   * @return The string in host byte order, or a kMemory error.
   */
  [[nodiscard]] Result<std::u16string> ReadString16(u32 address, u32 max_length = 4096) const;

  /**
   * Allocates guest memory.
   *
   * @param size The size in bytes.
   * @param alignment A power of two, at least 4096 for title memory.
   * @param heap The heap, title memory by default. Physical is the 64 KiB window at 0xA0000000.
   * @return The address, or a kMemory error.
   */
  [[nodiscard]] Result<u32> Allocate(u32 size, u32 alignment, GuestProtect protect,
                                     GuestHeap heap = GuestHeap::kTitle);

  /**
   * Frees memory from Allocate, from either heap.
   *
   * @param address The address Allocate returned.
   * @return Ok, or a kMemory error.
   */
  [[nodiscard]] Result<void> Free(u32 address);

  /**
   * Changes the access of mapped pages. The range covers every page it touches.
   *
   * @return Ok, or a kMemory error when any page is not mapped.
   */
  [[nodiscard]] Result<void> Protect(u32 address, u32 length, GuestProtect protect);

  /**
   * Returns an address view usable from any thread until TitleRuntime::Unload.
   */
  [[nodiscard]] kernel::MemoryView GetView() const;

  /**
   * Translates a host pointer inside guest memory to its guest address.
   *
   * @param host A pointer, such as the data of a span from GetWritableSpan.
   * @return The guest address, or a kMemory error when the pointer is outside guest memory.
   */
  [[nodiscard]] Result<u32> HostToGuest(const void* host) const;

 private:
  friend class TitleRuntime;
  friend class kernel::GuestAllocator;
  friend class kernel::TitleState;
  explicit GuestMemory(kernel::Memory& memory);
  kernel::Memory& GetMemory() const;
  ~GuestMemory();

  struct State;
  State* state_ = nullptr;
};

/**
 * Reads a guest value.
 *
 * @return A copy, or a kMemory error.
 */
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] Result<T> GuestMemory::Load(u32 address) const {
  T value;
  REX_TRY_VOID(CopyFrom(address, std::as_writable_bytes(std::span(&value, 1))));
  return value;
}

/**
 * Writes a guest value.
 *
 * @return Ok, or a kMemory error.
 */
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] Result<void> GuestMemory::Store(u32 address, const T& value) {
  return CopyTo(address, std::as_bytes(std::span(&value, 1)));
}

}  // namespace rex::kernel
