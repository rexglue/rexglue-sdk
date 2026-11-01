/**
 * @file        utility.h
 * @brief       Move only function, const propagating pointer and scope guard
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <version>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

class MoveOnly {
 protected:
  MoveOnly() = default;
  MoveOnly(MoveOnly&&) = default;
  MoveOnly& operator=(MoveOnly&&) = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
};

class NonCopyable {
 protected:
  NonCopyable() = default;
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;
};

namespace detail {

/**
 * Owning holder for a callable that may be move only.
 *
 * @tparam Const Whether the call operator is const and calls the target as a const lvalue.
 */
template <bool Const, typename R, typename... Args>
class MoveOnlyCallable {
  static constexpr std::size_t kStorageSize = 3 * sizeof(void*);
  static constexpr std::size_t kStorageAlign = alignof(std::max_align_t);

  template <typename D>
  using TargetRef = std::conditional_t<Const, const D&, D&>;

  template <typename D>
  static constexpr bool kInline = sizeof(D) <= kStorageSize && alignof(D) <= kStorageAlign &&
                                  std::is_nothrow_move_constructible_v<D>;

  struct Ops {
    R (*invoke)(void* storage, Args&&... args);
    void (*relocate)(void* dst, void* src) noexcept;
    void (*destroy)(void* storage) noexcept;
  };

  template <typename D>
  static D* Target(void* storage) noexcept {
    if constexpr (kInline<D>) {
      return std::launder(static_cast<D*>(storage));
    } else {
      return *std::launder(static_cast<D**>(storage));
    }
  }

  template <typename D>
  static R Invoke(void* storage, Args&&... args) {
    TargetRef<D> target = *Target<D>(storage);
    if constexpr (std::is_void_v<R>) {
      static_cast<void>(std::invoke(target, std::forward<Args>(args)...));
    } else {
      return std::invoke(target, std::forward<Args>(args)...);
    }
  }

  template <typename D>
  static void Relocate(void* dst, void* src) noexcept {
    if constexpr (kInline<D>) {
      D* from = Target<D>(src);
      ::new (dst) D(std::move(*from));
      from->~D();
    } else {
      ::new (dst) D*(Target<D>(src));
    }
  }

  template <typename D>
  static void Destroy(void* storage) noexcept {
    if constexpr (kInline<D>) {
      Target<D>(storage)->~D();
    } else {
      delete Target<D>(storage);
    }
  }

  template <typename D>
  static constexpr Ops kOps{&Invoke<D>, &Relocate<D>, &Destroy<D>};

  template <typename F>
  static constexpr bool kAccepts =
      std::conjunction_v<std::negation<std::is_same<std::remove_cvref_t<F>, MoveOnlyCallable>>,
                         std::is_constructible<std::decay_t<F>, F>,
                         std::is_invocable_r<R, TargetRef<std::decay_t<F>>, Args...>>;

 public:
  MoveOnlyCallable() noexcept = default;

  MoveOnlyCallable(std::nullptr_t) noexcept {}

  template <typename F>
    requires kAccepts<F>
  MoveOnlyCallable(F&& f) {
    using D = std::decay_t<F>;
    if constexpr (std::is_pointer_v<D> || std::is_member_pointer_v<D>) {
      if (f == nullptr) {
        return;
      }
    }
    if constexpr (kInline<D>) {
      ::new (static_cast<void*>(storage_)) D(std::forward<F>(f));
    } else {
      ::new (static_cast<void*>(storage_)) D*(new D(std::forward<F>(f)));
    }
    ops_ = &kOps<D>;
  }

  MoveOnlyCallable(MoveOnlyCallable&& other) noexcept { TakeFrom(other); }

  MoveOnlyCallable(const MoveOnlyCallable&) = delete;
  MoveOnlyCallable& operator=(const MoveOnlyCallable&) = delete;

  ~MoveOnlyCallable() { Reset(); }

  MoveOnlyCallable& operator=(MoveOnlyCallable&& other) noexcept {
    if (this != &other) {
      Reset();
      TakeFrom(other);
    }
    return *this;
  }

  MoveOnlyCallable& operator=(std::nullptr_t) noexcept {
    Reset();
    return *this;
  }

  template <typename F>
    requires kAccepts<F>
  MoveOnlyCallable& operator=(F&& f) {
    *this = MoveOnlyCallable(std::forward<F>(f));
    return *this;
  }

  void swap(MoveOnlyCallable& other) noexcept {
    MoveOnlyCallable held(std::move(other));
    other = std::move(*this);
    *this = std::move(held);
  }

  friend void swap(MoveOnlyCallable& a, MoveOnlyCallable& b) noexcept { a.swap(b); }

  explicit operator bool() const noexcept { return ops_ != nullptr; }

  friend bool operator==(const MoveOnlyCallable& f, std::nullptr_t) noexcept {
    return f.ops_ == nullptr;
  }

  R operator()(Args... args)
    requires(!Const)
  {
    REX_ASSERT(ops_ != nullptr);
    return ops_->invoke(storage_, std::forward<Args>(args)...);
  }

  R operator()(Args... args) const
    requires Const
  {
    REX_ASSERT(ops_ != nullptr);
    return ops_->invoke(const_cast<std::byte*>(storage_), std::forward<Args>(args)...);
  }

 private:
  void TakeFrom(MoveOnlyCallable& other) noexcept {
    if (other.ops_ != nullptr) {
      other.ops_->relocate(storage_, other.storage_);
      ops_ = std::exchange(other.ops_, nullptr);
    }
  }

  void Reset() noexcept {
    if (const Ops* ops = std::exchange(ops_, nullptr)) {
      ops->destroy(storage_);
    }
  }

  alignas(kStorageAlign) std::byte storage_[kStorageSize]{};
  const Ops* ops_ = nullptr;
};

/**
 * Maps a call signature to its MoveOnlyCallable.
 *
 * @tparam Signature The call signature, R(Args...) or R(Args...) const.
 */
template <typename Signature>
struct MoveOnlyFunctionFor;

/** Maps R(Args...) to a holder whose call operator is not const. */
template <typename R, typename... Args>
struct MoveOnlyFunctionFor<R(Args...)> {
  using type = MoveOnlyCallable<false, R, Args...>;
};

/** Maps R(Args...) const to a holder whose call operator is const. */
template <typename R, typename... Args>
struct MoveOnlyFunctionFor<R(Args...) const> {
  using type = MoveOnlyCallable<true, R, Args...>;
};

}  // namespace detail

#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L

/**
 * Owning holder for a callable that may be move only.
 *
 * @tparam Signature The call signature, R(Args...) or R(Args...) const.
 */
template <typename Signature>
using MoveOnlyFunction = std::move_only_function<Signature>;

#else

/**
 * Owning holder for a callable that may be move only.
 *
 * @tparam Signature The call signature, R(Args...) or R(Args...) const.
 */
template <typename Signature>
using MoveOnlyFunction = typename detail::MoveOnlyFunctionFor<Signature>::type;

#endif

/**
 * Pointer wrapper whose pointee follows the const of its owner.
 *
 * @tparam T A raw pointer or a smart pointer such as std::unique_ptr.
 */
template <typename T>
class PropagateConst : public MoveOnly {
 public:
  using element_type = std::remove_reference_t<decltype(*std::declval<T&>())>;

  PropagateConst() = default;

  PropagateConst(T pointer) noexcept(std::is_nothrow_move_constructible_v<T>)
      : pointer_(std::move(pointer)) {}

  PropagateConst(PropagateConst&& other) noexcept = default;

  PropagateConst& operator=(PropagateConst&& other) noexcept = default;

  PropagateConst& operator=(T pointer) noexcept(std::is_nothrow_move_assignable_v<T>) {
    pointer_ = std::move(pointer);
    return *this;
  }

  element_type* get() noexcept { return std::to_address(pointer_); }

  const element_type* get() const noexcept { return std::to_address(pointer_); }

  element_type* operator->() noexcept { return get(); }

  const element_type* operator->() const noexcept { return get(); }

  element_type& operator*() noexcept { return *get(); }

  const element_type& operator*() const noexcept { return *get(); }

  explicit operator bool() const noexcept { return get() != nullptr; }

  friend bool operator==(const PropagateConst& value, std::nullptr_t) noexcept {
    return value.get() == nullptr;
  }

 private:
  T pointer_{};
};

/** Runs a function once when it is destroyed, reset or assigned over. Move only and nullable. */
class REX_API ScopeGuard {
 public:
  ScopeGuard() = default;

  /**
   * Creates a guard that runs a function.
   *
   * @param release Runs once, on destruction, Reset or reassignment.
   */
  explicit ScopeGuard(MoveOnlyFunction<void()> release);

  /**
   * Takes the function of another guard.
   *
   * @param other The guard to take from, which then does nothing.
   */
  ScopeGuard(ScopeGuard&& other) noexcept;

  /**
   * Runs this function and takes the function of another guard.
   *
   * @param other The guard to take from, which then does nothing.
   */
  ScopeGuard& operator=(ScopeGuard&& other) noexcept;

  ~ScopeGuard();

  /** Runs the function now and leaves the guard doing nothing. */
  void Reset();

 private:
  MoveOnlyFunction<void()> release_;
};
}  // namespace rex
