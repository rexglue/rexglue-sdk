/**
 * @file        move_only_function.h
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
#include <new>
#include <type_traits>
#include <utility>
#include <version>

#include <rex/assert.h>

namespace rex {

namespace detail {

/**
 * Owning holder for a callable that may be move only, used where the standard library lacks
 * std::move_only_function.
 *
 * @tparam Const Whether the call operator is const and calls the target as a const lvalue.
 * @tparam R The return type.
 * @tparam Args The parameter types.
 */
template <bool Const, typename R, typename... Args>
class MoveOnlyFunctionImpl {
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
      std::conjunction_v<std::negation<std::is_same<std::remove_cvref_t<F>, MoveOnlyFunctionImpl>>,
                         std::is_constructible<std::decay_t<F>, F>,
                         std::is_invocable_r<R, TargetRef<std::decay_t<F>>, Args...>>;

 public:
  /** Constructs an empty holder. */
  MoveOnlyFunctionImpl() noexcept = default;

  /** Constructs an empty holder. */
  MoveOnlyFunctionImpl(std::nullptr_t) noexcept {}

  /**
   * Constructs a holder that owns a decayed copy of f. A null function or member pointer gives an
   * empty holder.
   *
   * @tparam F The callable type.
   * @param f The callable to take.
   */
  template <typename F>
    requires kAccepts<F>
  MoveOnlyFunctionImpl(F&& f) {
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

  /**
   * Takes the target of other and leaves other empty.
   *
   * @param other The holder to move from.
   */
  MoveOnlyFunctionImpl(MoveOnlyFunctionImpl&& other) noexcept { TakeFrom(other); }

  MoveOnlyFunctionImpl(const MoveOnlyFunctionImpl&) = delete;
  MoveOnlyFunctionImpl& operator=(const MoveOnlyFunctionImpl&) = delete;

  ~MoveOnlyFunctionImpl() { Reset(); }

  /**
   * Destroys the current target and takes the target of other.
   *
   * @param other The holder to move from.
   *
   * @return This holder.
   */
  MoveOnlyFunctionImpl& operator=(MoveOnlyFunctionImpl&& other) noexcept {
    if (this != &other) {
      Reset();
      TakeFrom(other);
    }
    return *this;
  }

  /**
   * Destroys the current target.
   *
   * @return This holder.
   */
  MoveOnlyFunctionImpl& operator=(std::nullptr_t) noexcept {
    Reset();
    return *this;
  }

  /**
   * Replaces the current target with a decayed copy of f.
   *
   * @tparam F The callable type.
   * @param f The callable to take.
   *
   * @return This holder.
   */
  template <typename F>
    requires kAccepts<F>
  MoveOnlyFunctionImpl& operator=(F&& f) {
    *this = MoveOnlyFunctionImpl(std::forward<F>(f));
    return *this;
  }

  /**
   * Exchanges the targets of 2 holders.
   *
   * @param other The holder to exchange with.
   */
  void swap(MoveOnlyFunctionImpl& other) noexcept {
    MoveOnlyFunctionImpl held(std::move(other));
    other = std::move(*this);
    *this = std::move(held);
  }

  /**
   * Exchanges the targets of 2 holders.
   *
   * @param a The first holder.
   * @param b The second holder.
   */
  friend void swap(MoveOnlyFunctionImpl& a, MoveOnlyFunctionImpl& b) noexcept { a.swap(b); }

  /**
   * Reports whether the holder has a target.
   *
   * @return True when the holder has a target.
   */
  explicit operator bool() const noexcept { return ops_ != nullptr; }

  /**
   * Reports whether a holder is empty.
   *
   * @param f The holder.
   *
   * @return True when f has no target.
   */
  friend bool operator==(const MoveOnlyFunctionImpl& f, std::nullptr_t) noexcept {
    return f.ops_ == nullptr;
  }

  /**
   * Calls the target as an lvalue.
   *
   * @param args The arguments to forward.
   *
   * @return The target's result converted to R.
   */
  R operator()(Args... args)
    requires(!Const)
  {
    REX_ASSERT(ops_ != nullptr);
    return ops_->invoke(storage_, std::forward<Args>(args)...);
  }

  /**
   * Calls the target as a const lvalue.
   *
   * @param args The arguments to forward.
   *
   * @return The target's result converted to R.
   */
  R operator()(Args... args) const
    requires Const
  {
    REX_ASSERT(ops_ != nullptr);
    return ops_->invoke(const_cast<std::byte*>(storage_), std::forward<Args>(args)...);
  }

 private:
  void TakeFrom(MoveOnlyFunctionImpl& other) noexcept {
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
 * Maps a call signature to its MoveOnlyFunctionImpl.
 *
 * @tparam Signature The call signature, R(Args...) or R(Args...) const.
 */
template <typename Signature>
struct MoveOnlyFunctionFor;

/**
 * Maps R(Args...) to a holder whose call operator is not const.
 *
 * @tparam R The return type.
 * @tparam Args The parameter types.
 */
template <typename R, typename... Args>
struct MoveOnlyFunctionFor<R(Args...)> {
  /** The holder type. */
  using type = MoveOnlyFunctionImpl<false, R, Args...>;
};

/**
 * Maps R(Args...) const to a holder whose call operator is const.
 *
 * @tparam R The return type.
 * @tparam Args The parameter types.
 */
template <typename R, typename... Args>
struct MoveOnlyFunctionFor<R(Args...) const> {
  /** The holder type. */
  using type = MoveOnlyFunctionImpl<true, R, Args...>;
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

}  // namespace rex
