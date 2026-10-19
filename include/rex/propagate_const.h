/**
 * @file        propagate_const.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

#include <rex/types.h>

namespace rex {

/**
 * Pointer wrapper whose pointee follows the const of its owner, so a const member function cannot
 * reach a non-const member of an object held through a pointer.
 *
 * @tparam T A raw pointer or a smart pointer such as std::unique_ptr.
 */
template <typename T>
class PropagateConst : public MoveOnly {
 public:
  /** The type the pointer points to. */
  using element_type = std::remove_reference_t<decltype(*std::declval<T&>())>;

  /** Holds no pointer. */
  PropagateConst() = default;

  /**
   * Takes the pointer.
   *
   * @param pointer The pointer to hold.
   */
  PropagateConst(T pointer) noexcept(std::is_nothrow_move_constructible_v<T>)
      : pointer_(std::move(pointer)) {}

  /**
   * Takes the pointer of another wrapper.
   *
   * @param other The wrapper to take the pointer from.
   */
  PropagateConst(PropagateConst&& other) noexcept = default;

  /**
   * Takes the pointer of another wrapper.
   *
   * @param other The wrapper to take the pointer from.
   * @return This wrapper.
   */
  PropagateConst& operator=(PropagateConst&& other) noexcept = default;

  /**
   * Replaces the held pointer.
   *
   * @param pointer The pointer to hold.
   * @return This wrapper.
   */
  PropagateConst& operator=(T pointer) noexcept(std::is_nothrow_move_assignable_v<T>) {
    pointer_ = std::move(pointer);
    return *this;
  }

  /**
   * Returns the pointee for non-const access.
   *
   * @return The pointee, null when empty.
   */
  element_type* get() noexcept { return std::to_address(pointer_); }

  /**
   * Returns the pointee for const access.
   *
   * @return The pointee, null when empty.
   */
  const element_type* get() const noexcept { return std::to_address(pointer_); }

  /**
   * Accesses a member of the pointee.
   *
   * @return The pointee.
   */
  element_type* operator->() noexcept { return get(); }

  /**
   * Accesses a member of the pointee as const.
   *
   * @return The pointee.
   */
  const element_type* operator->() const noexcept { return get(); }

  /**
   * Dereferences the pointee.
   *
   * @return The pointee.
   */
  element_type& operator*() noexcept { return *get(); }

  /**
   * Dereferences the pointee as const.
   *
   * @return The pointee.
   */
  const element_type& operator*() const noexcept { return *get(); }

  /**
   * Reports whether a pointer is held.
   *
   * @return True when the held pointer is not null.
   */
  explicit operator bool() const noexcept { return get() != nullptr; }

  /**
   * Reports whether the wrapper is empty.
   *
   * @param value The wrapper to test.
   * @return True when the held pointer is null.
   */
  friend bool operator==(const PropagateConst& value, std::nullptr_t) noexcept {
    return value.get() == nullptr;
  }

 private:
  T pointer_{};
};

}  // namespace rex
