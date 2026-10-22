/**
 * @file        runtime/guest_global.h
 * @brief       A typed global the title keeps at a fixed guest address
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <type_traits>

#include <rex/assert.h>
#include <rex/runtime/memory_view.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex {

/**
 * Binds a type to a guest address. It holds no state, and every access resolves the address
 * through guest memory, so the title owns the value.
 *
 * @tparam T The type the title keeps at the address, laid out as the title stores it.
 * @tparam Address The guest address of the value.
 */
template <typename T, u32 Address>
struct GuestGlobal {
  static_assert(Address != 0, "A guest global needs a nonzero address");
  static_assert(Address % alignof(T) == 0, "A guest global address must be aligned for its type");
  static_assert(std::is_trivially_copyable_v<T>, "A guest global holds a trivially copyable type");

  /**
   * Returns the guest address.
   *
   * @return The guest address of the value.
   */
  static constexpr u32 GetAddress() { return Address; }

  /**
   * Resolves the value through the guest memory of the calling thread.
   *
   * @return The host pointer to the value.
   */
  T* get() const {
    auto* thread_state = rex::runtime::ThreadState::Get();
    REX_ASSERT(thread_state != nullptr);
    return rex::runtime::GuestPtr<T*>(thread_state->GetMemory()->GetVirtualMembase(), Address);
  }

  /**
   * Resolves the value through a memory view, for a host thread with no bound runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer to the value.
   */
  T* get(const rex::runtime::MemoryView& view) const { return view.TranslateVirtual<T*>(Address); }

  /**
   * Resolves the value through the guest memory of the calling thread only when the title could
   * read it.
   *
   * @return The host pointer, null when the address is in no heap, its page is not readable, or
   * the calling thread has no runtime.
   */
  T* TryGet() const {
    auto* thread_state = rex::runtime::ThreadState::Get();
    if (thread_state == nullptr || thread_state->GetMemory() == nullptr) {
      return nullptr;
    }
    return thread_state->GetMemory()->TryTranslateVirtual<T*>(Address);
  }

  /**
   * Resolves the value through a memory view only when the title could read it, for a host thread
   * with no bound runtime.
   *
   * @param view The memory view that holds the guest memory.
   * @return The host pointer, null when the address is in no heap or its page is not readable.
   */
  T* TryGet(const rex::runtime::MemoryView& view) const {
    return view.TryTranslateVirtual<T*>(Address);
  }

  /**
   * Resolves the value through the guest memory of the calling thread.
   *
   * @return The host pointer to the value.
   */
  T* operator->() const { return get(); }

  /**
   * Resolves the value through the guest memory of the calling thread.
   *
   * @return The value.
   */
  T& operator*() const { return *get(); }
};

}  // namespace rex

/**
 * Declares a name for a global the title keeps at a fixed guest address, at namespace scope. A type
 * with a comma, such as a template with 2 arguments, needs an alias first.
 *
 * @param type The type the title keeps at the address.
 * @param name The name of the binding.
 * @param address The guest address, a nonzero literal aligned for the type.
 *
 * @code
 * REX_GUEST_GLOBAL(be_u32, kFrameCount, 0x82001234);
 * u32 frames = *kFrameCount.get();
 * @endcode
 */
#define REX_GUEST_GLOBAL(type, name, address) \
  inline constexpr rex::GuestGlobal<type, address> name {}
