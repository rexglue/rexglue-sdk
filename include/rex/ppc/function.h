/**
 * @file        ppc/function.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

/*
 * Parts of the translation wrappers are based on XenonRecomp and UnleashedRecomp.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/logging/flush.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/platform/exceptions.h>
#include <rex/ppc/context.h>
#include <rex/runtime/thread_state.h>
#include <rex/runtime/xmemory.h>
#include <rex/types.h>

namespace rex::ppc {

/**
 * Returns a default constructed tuple of the argument types of a function pointer.
 *
 * @tparam R The return type.
 * @tparam T The argument types.
 * @return The tuple of argument types.
 */
template <typename R, typename... T>
constexpr std::tuple<T...> FunctionArgs(R (*)(T...)) noexcept {
  return std::tuple<T...>();
}

/**
 * The value V as a constexpr variable.
 *
 * @tparam V The value.
 */
template <auto V>
static constexpr decltype(V) constant_v = V;

template <typename T>
static constexpr bool is_float_v = std::is_same_v<T, f32> || std::is_same_v<T, f64>;

template <typename T>
concept BigEndianType = is_be_type_v<T>;

template <typename T>
concept MappedPtrType = is_mapped_ptr_v<T>;

template <typename T>
concept FloatType = is_float_v<T>;

/** A 128 bit vector value, which travels in v1 to v13 or in a 16 byte aligned stack slot pair. */
template <typename T>
concept VectorType = std::is_same_v<std::remove_cvref_t<T>, PPCVRegister>;

/**
 * A structure passed or returned by value in guest layout.
 *
 * @tparam T A trivially copyable structure made of be_ fields.
 */
template <typename T>
struct ByValue {
  T value; /**< The structure. */
};

/**
 * Reports whether a type is a ByValue structure.
 *
 * @tparam T The type.
 */
template <typename T>
struct IsByValue : std::false_type {};

/**
 * A ByValue structure.
 *
 * @tparam T The structure a ByValue holds.
 */
template <typename T>
struct IsByValue<ByValue<T>> : std::true_type {};

/** A ByValue structure. */
template <typename T>
concept ByValueType = IsByValue<std::remove_cvref_t<T>>::value;

/**
 * Size in bytes of the structure a ByValue holds.
 *
 * @tparam T The ByValue type.
 */
template <ByValueType T>
inline constexpr size_t kByValueSize = sizeof(std::declval<std::remove_cvref_t<T>>().value);

/** A ByValue structure over 8 bytes, returned through a pointer the caller passes. */
template <typename T>
concept HiddenReturnType = ByValueType<T> && kByValueSize<T> > sizeof(u64);

/**
 * Returns the number of 8 byte image slots a structure takes.
 *
 * @param size The structure size in bytes.
 * @return The size rounded up to whole slots.
 */
constexpr u32 ByValueSlots(size_t size) {
  return static_cast<u32>((size + sizeof(u64) - 1) / sizeof(u64));
}

/**
 * Reports whether a structure sits at the low end of its slot instead of the start.
 *
 * @param size The structure size in bytes.
 * @return True when the size is a power of 2 under 8.
 */
constexpr bool IsRightJustified(size_t size) {
  return size < sizeof(u64) && std::has_single_bit(size);
}

/** A plain type that is not a pointer, a big endian type, a MappedPtr, a vector or a structure. */
template <typename T>
concept PlainType = !std::is_pointer_v<T> && !BigEndianType<T> && !MappedPtrType<T> &&
                    !VectorType<T> && !ByValueType<T>;

/**
 * Number of parameters of a function.
 *
 * @tparam Func The function.
 */
template <auto Func>
struct ArgCountT {
  /** Number of parameters. */
  static constexpr size_t value = std::tuple_size_v<decltype(FunctionArgs(Func))>;
};

/** Where 1 argument travels in the guest calling convention. */
struct ArgPlace {
  u32 slot{};   /**< First 8 byte image slot the argument takes. */
  u32 slots{};  /**< Number of slots the argument takes. */
  i32 gpr{-1};  /**< First integer register index, 0 for r3, or -1 when it is not in one. */
  i32 fpr{-1};  /**< Float register index, 0 for f1, or -1. */
  i32 vr{-1};   /**< Vector register index, 0 for v1, or -1. */
  bool stack{}; /**< True when any part of the argument is in the stack image. */
};

/** Reads and writes function arguments in the guest calling convention. */
struct ArgTranslator {
  /** Number of integer argument registers, r3 to r10. */
  static constexpr size_t kIntegerRegisters = 8;
  /** Number of float argument registers, f1 to f13. */
  static constexpr size_t kFloatRegisters = 13;
  /** Number of vector argument registers, v1 to v13. */
  static constexpr size_t kVectorRegisters = 13;

  /**
   * Returns the integer argument register for an argument image slot.
   *
   * @param ctx The register file.
   * @param slot The slot, under kIntegerRegisters.
   * @return r3 for slot 0 through r10 for slot 7.
   */
  static constexpr PPCRegister& GetIntegerRegister(PPCContext& ctx, size_t slot) noexcept {
    switch (slot) {
      case 0:
        return ctx.r3;
      case 1:
        return ctx.r4;
      case 2:
        return ctx.r5;
      case 3:
        return ctx.r6;
      case 4:
        return ctx.r7;
      case 5:
        return ctx.r8;
      case 6:
        return ctx.r9;
      default:
        return ctx.r10;
    }
  }

  /**
   * Returns a float argument register.
   *
   * @param ctx The register file.
   * @param ordinal The float argument index, under kFloatRegisters.
   * @return f1 for index 0 through f13 for index 12.
   */
  static constexpr PPCRegister& GetFloatRegister(PPCContext& ctx, size_t ordinal) noexcept {
    switch (ordinal) {
      case 0:
        return ctx.f1;
      case 1:
        return ctx.f2;
      case 2:
        return ctx.f3;
      case 3:
        return ctx.f4;
      case 4:
        return ctx.f5;
      case 5:
        return ctx.f6;
      case 6:
        return ctx.f7;
      case 7:
        return ctx.f8;
      case 8:
        return ctx.f9;
      case 9:
        return ctx.f10;
      case 10:
        return ctx.f11;
      case 11:
        return ctx.f12;
      default:
        return ctx.f13;
    }
  }

  /**
   * Returns a vector argument register.
   *
   * @param ctx The register file.
   * @param ordinal The vector argument index, under kVectorRegisters.
   * @return v1 for index 0 through v13 for index 12.
   */
  static constexpr PPCVRegister& GetVectorRegister(PPCContext& ctx, size_t ordinal) noexcept {
    PPCVRegister* registers[] = {&ctx.v1, &ctx.v2, &ctx.v3,  &ctx.v4,  &ctx.v5,  &ctx.v6, &ctx.v7,
                                 &ctx.v8, &ctx.v9, &ctx.v10, &ctx.v11, &ctx.v12, &ctx.v13};
    return *registers[ordinal];
  }

  /**
   * Returns the guest address of an argument image slot past the integer registers.
   *
   * @param ctx The register file at the callee's entry.
   * @param slot The slot, at least kIntegerRegisters.
   * @return The address of the slot's double word.
   */
  static constexpr u32 GetStackSlotAddress(const PPCContext& ctx, size_t slot) noexcept {
    return ctx.r1.u32 + 0x50 + static_cast<u32>((slot - kIntegerRegisters) * 8);
  }

  /**
   * Reads an integer argument from its register or its stack slot.
   *
   * @tparam Bytes The argument width in bytes, 4 or 8.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param slot The argument image slot.
   * @return The argument, zero extended.
   */
  template <size_t Bytes = sizeof(u32)>
  static constexpr u64 GetIntegerArgumentValue(const PPCContext& ctx, u8* base,
                                               size_t slot) noexcept {
    if constexpr (Bytes == sizeof(u64)) {
      if (slot < kIntegerRegisters) {
        return GetIntegerRegister(const_cast<PPCContext&>(ctx), slot).u64;
      }
      return *rex::runtime::GuestPtr<be_u64*>(base, GetStackSlotAddress(ctx, slot));
    } else {
      if (slot < kIntegerRegisters) {
        return GetIntegerRegister(const_cast<PPCContext&>(ctx), slot).u32;
      }
      return *rex::runtime::GuestPtr<be_u32*>(base, GetStackSlotAddress(ctx, slot) + 4);
    }
  }

  /**
   * Reads a float argument from its register or its stack slot.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @param single True when the argument is a 4 byte float.
   * @return The argument widened to a double.
   */
  static f64 GetFloatArgumentValue(const PPCContext& ctx, u8* base, const ArgPlace& arg,
                                   bool single) noexcept {
    if (arg.fpr >= 0) {
      return GetFloatRegister(const_cast<PPCContext&>(ctx), static_cast<size_t>(arg.fpr)).f64;
    }
    const u32 address = GetStackSlotAddress(ctx, arg.slot);
    if (single) {
      return std::bit_cast<f32>(
          static_cast<u32>(*rex::runtime::GuestPtr<be_u32*>(base, address + 4)));
    }
    return std::bit_cast<f64>(static_cast<u64>(*rex::runtime::GuestPtr<be_u64*>(base, address)));
  }

  /**
   * Writes an integer argument to its register or its stack slot.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   * @tparam Bytes The argument width in bytes, 4 or 8.
   * @param slot The argument image slot.
   * @param value The value to write, a 4 byte value goes to the low word of a stack slot.
   */
  template <size_t Bytes = sizeof(u32)>
  static constexpr void SetIntegerArgumentValue(PPCContext& ctx, u8* base, size_t slot,
                                                u64 value) noexcept {
    if (slot < kIntegerRegisters) {
      GetIntegerRegister(ctx, slot).u64 = value;
      return;
    }
    if constexpr (Bytes == sizeof(u64)) {
      *rex::runtime::GuestPtr<be_u64*>(base, GetStackSlotAddress(ctx, slot)) = value;
    } else {
      *rex::runtime::GuestPtr<be_u32*>(base, GetStackSlotAddress(ctx, slot) + 4) =
          static_cast<u32>(value);
    }
  }

  /**
   * Writes a float argument to its register, and to its stack slot when its slot is past r10.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @param value The value to write.
   * @param single True when the argument is a 4 byte float.
   */
  static void SetFloatArgumentValue(PPCContext& ctx, u8* base, const ArgPlace& arg, f64 value,
                                    bool single) noexcept {
    if (arg.fpr >= 0) {
      GetFloatRegister(ctx, static_cast<size_t>(arg.fpr)).f64 = value;
      if (!arg.stack) {
        return;
      }
    }
    const u32 address = GetStackSlotAddress(ctx, arg.slot);
    if (single) {
      *rex::runtime::GuestPtr<be_u32*>(base, address + 4) =
          std::bit_cast<u32>(static_cast<f32>(value));
    } else {
      *rex::runtime::GuestPtr<be_u64*>(base, address) = std::bit_cast<u64>(value);
    }
  }

  /**
   * Reads a vector argument from its register or its 16 byte stack slot pair.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The vector, whose stack image is the byte reversed register.
   */
  static PPCVRegister GetVectorArgumentValue(const PPCContext& ctx, u8* base,
                                             const ArgPlace& arg) noexcept {
    if (arg.vr >= 0) {
      return GetVectorRegister(const_cast<PPCContext&>(ctx), static_cast<size_t>(arg.vr));
    }
    PPCVRegister result{};
    const u8* image = rex::runtime::GuestPtr<const u8*>(base, GetStackSlotAddress(ctx, arg.slot));
    for (size_t i = 0; i < sizeof(PPCVRegister); ++i) {
      result.u8[i] = image[sizeof(PPCVRegister) - 1 - i];
    }
    return result;
  }

  /**
   * Writes a vector argument to its register or its 16 byte stack slot pair.
   *
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @param value The vector, stored byte reversed in the stack image.
   */
  static void SetVectorArgumentValue(PPCContext& ctx, u8* base, const ArgPlace& arg,
                                     const PPCVRegister& value) noexcept {
    if (arg.vr >= 0) {
      GetVectorRegister(ctx, static_cast<size_t>(arg.vr)) = value;
      return;
    }
    u8* image = rex::runtime::GuestPtr<u8*>(base, GetStackSlotAddress(ctx, arg.slot));
    for (size_t i = 0; i < sizeof(PPCVRegister); ++i) {
      image[sizeof(PPCVRegister) - 1 - i] = value.u8[i];
    }
  }

  /**
   * Reads a structure argument from its registers and stack slots.
   *
   * @tparam T The ByValue type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The structure.
   */
  template <ByValueType T>
  static T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    constexpr size_t kSize = kByValueSize<T>;
    u8 image[ByValueSlots(kSize) * sizeof(u64)]{};
    for (u32 k = 0; k < arg.slots; ++k) {
      const size_t slot = arg.slot + k;
      const u64 word = slot < kIntegerRegisters ? GetIntegerRegister(ctx, slot).u64
                                                : static_cast<u64>(*rex::runtime::GuestPtr<be_u64*>(
                                                      base, GetStackSlotAddress(ctx, slot)));
      for (size_t b = 0; b < sizeof(u64); ++b) {
        image[k * sizeof(u64) + b] = static_cast<u8>(word >> (56 - 8 * b));
      }
    }
    T result{};
    std::memcpy(&result.value, image + (IsRightJustified(kSize) ? sizeof(u64) - kSize : 0), kSize);
    return result;
  }

  /**
   * Writes a structure argument to its registers and stack slots.
   *
   * @tparam T The ByValue type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @param value The structure.
   */
  template <ByValueType T>
  static void SetByValueArgumentValue(PPCContext& ctx, u8* base, const ArgPlace& arg,
                                      const T& value) noexcept {
    constexpr size_t kSize = kByValueSize<T>;
    u8 image[ByValueSlots(kSize) * sizeof(u64)]{};
    std::memcpy(image + (IsRightJustified(kSize) ? sizeof(u64) - kSize : 0), &value.value, kSize);
    for (u32 k = 0; k < arg.slots; ++k) {
      u64 word = 0;
      for (size_t b = 0; b < sizeof(u64); ++b) {
        word = (word << 8) | image[k * sizeof(u64) + b];
      }
      const size_t slot = arg.slot + k;
      if (slot < kIntegerRegisters) {
        GetIntegerRegister(ctx, slot).u64 = word;
      } else {
        *rex::runtime::GuestPtr<be_u64*>(base, GetStackSlotAddress(ctx, slot)) = word;
      }
    }
  }

  /**
   * Reads a big endian argument.
   *
   * @tparam T The big endian type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The argument.
   */
  template <BigEndianType T>
  static constexpr T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    T result;
    result.value = static_cast<decltype(result.value)>(
        GetIntegerArgumentValue<sizeof(result.value)>(ctx, base, arg.slot));
    return result;
  }

  /**
   * Reads a mapped pointer argument.
   *
   * @tparam T The mapped pointer type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The mapped pointer, null when the argument is 0.
   */
  template <MappedPtrType T>
  static T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    return GetMappedPointerValue<T>(GetIntegerArgumentValue(ctx, base, arg.slot), base);
  }

  /**
   * Converts the value of an integer register to a mapped pointer.
   *
   * @tparam T The mapped pointer type.
   * @param value The register value, its low word is the guest address.
   * @param base The guest memory base.
   * @return The mapped pointer, null when the address is 0.
   */
  template <MappedPtrType T>
  static T GetMappedPointerValue(u64 value, u8* base) noexcept {
    using InnerT = MappedPtrInnerType<T>::type;
    const u32 guest_addr = static_cast<u32>(value);
    if (guest_addr == 0) {
      return T(nullptr);
    }
    return T(rex::runtime::GuestPtr<InnerT*>(base, guest_addr), guest_addr);
  }

  /**
   * Reads a vector argument.
   *
   * @tparam T The vector type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The vector.
   */
  template <VectorType T>
  static T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    return GetVectorArgumentValue(ctx, base, arg);
  }

  /**
   * Reads an integer or float argument.
   *
   * @tparam T The argument type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The argument.
   */
  template <PlainType T>
  static constexpr T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    if constexpr (is_float_v<T>) {
      return static_cast<T>(GetFloatArgumentValue(ctx, base, arg, std::is_same_v<T, f32>));
    } else {
      return static_cast<T>(GetIntegerArgumentValue<sizeof(T)>(ctx, base, arg.slot));
    }
  }

  /**
   * Reads a pointer argument.
   *
   * @tparam T The pointer type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @return The host pointer, null when the argument is 0.
   */
  template <typename T>
    requires std::is_pointer_v<T>
  static constexpr T GetValue(PPCContext& ctx, u8* base, const ArgPlace& arg) noexcept {
    return GetPointerValue<T>(GetIntegerArgumentValue(ctx, base, arg.slot), base);
  }

  /**
   * Converts the value of an integer register to a host pointer.
   *
   * @tparam T The pointer type.
   * @param value The register value, its low word is the guest address.
   * @param base The guest memory base.
   * @return The host pointer, null when the address is 0.
   */
  template <typename T>
    requires std::is_pointer_v<T>
  static constexpr T GetPointerValue(u64 value, u8* base) noexcept {
    const u32 guest_addr = static_cast<u32>(value);
    if (guest_addr == 0) {
      return nullptr;
    }
    return rex::runtime::GuestPtr<T>(base, guest_addr);
  }

  /**
   * Writes an argument of any supported type.
   *
   * @tparam T The argument type.
   * @param ctx The register file.
   * @param base The guest memory base.
   * @param arg The argument places.
   * @param value The value to write.
   */
  template <typename T>
  static constexpr void SetValue(PPCContext& ctx, u8* base, const ArgPlace& arg, T value) noexcept {
    if constexpr (is_float_v<T>) {
      SetFloatArgumentValue(ctx, base, arg, value, std::is_same_v<T, f32>);
    } else if constexpr (VectorType<T>) {
      SetVectorArgumentValue(ctx, base, arg, value);
    } else if constexpr (ByValueType<T>) {
      SetByValueArgumentValue(ctx, base, arg, value);
    } else if constexpr (MappedPtrType<T>) {
      SetIntegerArgumentValue(ctx, base, arg.slot, value.GetGuestAddress());
    } else if constexpr (std::is_null_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, arg.slot, 0);
    } else if constexpr (std::is_pointer_v<T>) {
      SetIntegerArgumentValue(
          ctx, base, arg.slot,
          rex::runtime::HostPtrToGuest(base, reinterpret_cast<const void*>(value)));
    } else {
      SetIntegerArgumentValue<(sizeof(T) > sizeof(u32)) ? sizeof(u64) : sizeof(u32)>(
          ctx, base, arg.slot, value);
    }
  }
};

/**
 * Reports whether a type is float or double.
 *
 * @tparam T The type.
 * @return True when T is float or double.
 */
template <typename T>
constexpr bool IsFloatType() {
  return is_float_v<T>;
}

/**
 * Places an argument list by the guest calling convention.
 *
 * An integer, pointer or big endian value takes the next slot and the next of r3 to r10. A float
 * takes the next slot and the next of f1 to f13, and leaves its slot in r3 to r10 unused. A float
 * whose slot is past r10 is written to that slot. A vector takes the next of v1 to v13 and
 * no slot. A vector without a register takes 2 slots from the first even slot at or past the first
 * stack slot. A structure takes whole slots from the next slot, split between r3 to r10 and the
 * stack when it crosses the 8th slot.
 *
 * @tparam Args The argument types in order.
 * @return A placement per argument.
 */
template <typename... Args>
constexpr std::array<ArgPlace, sizeof...(Args)> PlaceArgs() {
  std::array<ArgPlace, sizeof...(Args)> places{};
  if constexpr (sizeof...(Args) > 0) {
    constexpr u32 kIntegerRegisters = 8;
    constexpr u32 kFloatRegisters = 13;
    constexpr u32 kVectorRegisters = 13;
    u32 slot = 0;
    u32 float_register = 0;
    u32 vector_register = 0;
    size_t index = 0;
    (
        [&]<typename T>() {
          ArgPlace place;
          place.slot = slot;
          place.slots = 1;
          if constexpr (VectorType<T>) {
            if (vector_register < kVectorRegisters) {
              place.vr = static_cast<i32>(vector_register);
              place.slots = 0;
            } else {
              const u32 first = (std::max(slot, kIntegerRegisters) + 1) & ~1u;
              place.slot = first;
              place.slots = 2;
              place.stack = true;
              slot = first + place.slots;
            }
            ++vector_register;
            places[index++] = place;
            return;
          } else if constexpr (ByValueType<T>) {
            place.slots = ByValueSlots(kByValueSize<T>);
            if (slot < kIntegerRegisters) {
              place.gpr = static_cast<i32>(slot);
            }
            place.stack = slot + place.slots > kIntegerRegisters;
            slot += place.slots;
            places[index++] = place;
            return;
          } else if constexpr (IsFloatType<std::remove_cvref_t<T>>()) {
            if (float_register < kFloatRegisters) {
              place.fpr = static_cast<i32>(float_register);
            }
            place.stack = slot >= kIntegerRegisters || place.fpr < 0;
            ++float_register;
          } else {
            if (slot < kIntegerRegisters) {
              place.gpr = static_cast<i32>(slot);
            }
            place.stack = slot >= kIntegerRegisters;
          }
          ++slot;
          places[index++] = place;
        }.template operator()<Args>(),
        ...);
  }
  return places;
}

/**
 * Places each argument of a function signature by the guest calling convention.
 *
 * @tparam R The return type.
 * @tparam Args The argument types.
 * @return The places of each argument.
 */
template <typename R, typename... Args>
constexpr auto PlaceSignatureArgs(R (*)(Args...)) {
  if constexpr (HiddenReturnType<R>) {
    const auto all = PlaceArgs<u32, Args...>();
    std::array<ArgPlace, sizeof...(Args)> places{};
    for (size_t i = 0; i < sizeof...(Args); ++i) {
      places[i] = all[i + 1];
    }
    return places;
  } else {
    return PlaceArgs<Args...>();
  }
}

/**
 * Places each argument of a function by the guest calling convention.
 *
 * @tparam Func The function.
 * @return The places of each argument.
 */
template <auto Func>
constexpr auto PlaceFunctionArgs() {
  return PlaceSignatureArgs(Func);
}

/**
 * Places of 1 argument of a function.
 *
 * @tparam Func The function.
 * @tparam I The argument index.
 */
template <auto Func, size_t I>
struct ArgPlaceT {
  /** Places of argument I. */
  static constexpr ArgPlace value = PlaceFunctionArgs<Func>()[I];
};

/**
 * Ends the translation of arguments to host values.
 *
 * @tparam Func The native function.
 * @tparam I The argument index.
 * @tparam TArgs The argument types.
 * @param ctx The register file.
 * @param base The guest memory base.
 */
template <auto Func, int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void TranslateArgsToHost([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] u8* base,
                         [[maybe_unused]] std::tuple<TArgs...>&) noexcept {}

/**
 * Reads argument I and every later one from the register file into a tuple.
 *
 * @tparam Func The native function.
 * @tparam I The argument index.
 * @tparam TArgs The argument types.
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param tpl The argument tuple to fill.
 */
template <auto Func, int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void TranslateArgsToHost(PPCContext& ctx, u8* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  std::get<I>(tpl) = ArgTranslator::GetValue<T>(ctx, base, ArgPlaceT<Func, I>::value);
  TranslateArgsToHost<Func, I + 1>(ctx, base, tpl);
}

/**
 * Ends the translation of arguments to guest values.
 *
 * @tparam I The argument index.
 * @tparam TArgs The argument types.
 * @param ctx The register file.
 * @param base The guest memory base.
 */
template <int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void TranslateArgsToGuest([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] u8* base,
                          [[maybe_unused]] std::tuple<TArgs...>&) noexcept {}

/**
 * Writes argument I and every later one from a tuple into the register file and stack image.
 *
 * @tparam I The argument index.
 * @tparam TArgs The argument types.
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param tpl The argument tuple.
 */
template <int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void TranslateArgsToGuest(PPCContext& ctx, u8* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  ArgTranslator::SetValue<T>(ctx, base, PlaceArgs<TArgs...>()[I], std::get<I>(tpl));
  TranslateArgsToGuest<I + 1>(ctx, base, tpl);
}

/**
 * Writes a structure to guest memory in guest layout, a vector byte reversed.
 *
 * @tparam T The ByValue type.
 * @param base The guest memory base.
 * @param address The guest address.
 * @param value The structure.
 */
template <ByValueType T>
void StoreByValue(u8* base, u32 address, const T& value) {
  u8* destination = rex::runtime::GuestPtr<u8*>(base, address);
  if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value.value)>, PPCVRegister>) {
    for (size_t i = 0; i < sizeof(PPCVRegister); ++i) {
      destination[sizeof(PPCVRegister) - 1 - i] = value.value.u8[i];
    }
  } else {
    std::memcpy(destination, &value.value, sizeof(value.value));
  }
}

/**
 * Packs a structure of at most 8 bytes into the register image a function returns in r3.
 *
 * @tparam T The ByValue type.
 * @param value The structure.
 * @return The register value, a power of 2 size under 8 right justified and any other size left
 *         justified.
 */
template <ByValueType T>
u64 ByValueToWord(const T& value) {
  constexpr size_t kSize = kByValueSize<T>;
  u8 image[sizeof(u64)]{};
  std::memcpy(image + (IsRightJustified(kSize) ? sizeof(u64) - kSize : 0), &value.value, kSize);
  u64 word = 0;
  for (const u8 byte : image) {
    word = (word << 8) | byte;
  }
  return word;
}

/**
 * Unpacks a structure of at most 8 bytes from the register image a function returns in r3.
 *
 * @tparam T The ByValue type.
 * @param word The register value.
 * @return The structure.
 */
template <ByValueType T>
T ByValueFromWord(u64 word) {
  constexpr size_t kSize = kByValueSize<T>;
  u8 image[sizeof(u64)]{};
  for (size_t b = 0; b < sizeof(u64); ++b) {
    image[b] = static_cast<u8>(word >> (56 - 8 * b));
  }
  T result{};
  std::memcpy(&result.value, image + (IsRightJustified(kSize) ? sizeof(u64) - kSize : 0), kSize);
  return result;
}

/**
 * Reads a structure from guest memory in guest layout, a vector byte reversed.
 *
 * @tparam T The ByValue type.
 * @param base The guest memory base.
 * @param address The guest address.
 * @return The structure.
 */
template <ByValueType T>
T LoadByValue(u8* base, u32 address) {
  const u8* source = rex::runtime::GuestPtr<const u8*>(base, address);
  T result{};
  if constexpr (std::is_same_v<std::remove_cvref_t<decltype(result.value)>, PPCVRegister>) {
    for (size_t i = 0; i < sizeof(PPCVRegister); ++i) {
      result.value.u8[i] = source[sizeof(PPCVRegister) - 1 - i];
    }
  } else {
    std::memcpy(&result.value, source, sizeof(result.value));
  }
  return result;
}

/**
 * Writes the result of a native C++ function to the registers a guest caller reads.
 *
 * @tparam RetT The result type.
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param hidden_pointer The result pointer the caller passed in r3, used by a hidden return.
 * @param value The result.
 */
template <typename RetT>
void StoreHostResult(PPCContext& ctx, u8* base, u32 hidden_pointer, const RetT& value) {
  if constexpr (std::is_pointer<RetT>()) {
    ctx.r3.u64 = rex::runtime::HostPtrToGuest(base, reinterpret_cast<const void*>(value));
  } else if constexpr (is_float_v<RetT>) {
    ctx.f1.f64 = value;
  } else if constexpr (VectorType<RetT>) {
    ctx.v1 = value;
  } else if constexpr (HiddenReturnType<RetT>) {
    StoreByValue(base, hidden_pointer, value);
    ctx.r3.u64 = hidden_pointer;
  } else if constexpr (ByValueType<RetT>) {
    ctx.r3.u64 = ByValueToWord(value);
  } else {
    ctx.r3.u64 = static_cast<u64>(value);
  }
}

namespace detail {

/**
 * Checks in Debug builds that a hook runs on the register file of its thread.
 *
 * @param ctx The register file the hook was given.
 * @param name The hook, for the log.
 */
inline void CheckHookContext([[maybe_unused]] const PPCContext& ctx,
                             [[maybe_unused]] const char* name) {
#ifndef NDEBUG
  const auto* thread_state = rex::runtime::ThreadState::Get();
  if (thread_state && &ctx != thread_state->GetContext()) {
    REXLOG_ERROR(cpu, "Hook {} runs on a register file that is not its thread's", name);
  }
  REX_ASSERT(!thread_state || &ctx == thread_state->GetContext(),
             "A hook runs on a register file that is not its thread's");
#endif
}

}  // namespace detail

/**
 * Calls a native C++ function with arguments extracted from the PPC context. A compiler barrier
 * follows the call so the compiler does not reorder it.
 *
 * @tparam Func The native function.
 * @param ctx The register file.
 * @param base The guest memory base.
 */
template <auto Func>
__attribute__((noinline)) void GuestToHostThunk(PPCContext& ctx, u8* base) {
  using RetT = decltype(std::apply(Func, FunctionArgs(Func)));

  detail::CheckHookContext(ctx, __PRETTY_FUNCTION__);
  auto* thread_state = rex::runtime::ThreadState::Get();
  if (thread_state && thread_state->IsStopRequested()) {
    throw rex::ThreadStop{};
  }
#if !REX_PLATFORM_WIN32
  const rex::platform::HostCallDepth host_call_depth;
#endif
  const u32 hidden_pointer = ctx.r3.u32;
  auto args = FunctionArgs(Func);
  TranslateArgsToHost<Func>(ctx, base, args);

  if constexpr (std::is_same_v<RetT, void>) {
    std::apply(Func, args);
  } else {
    auto v = std::apply(Func, args);

    asm volatile("" ::: "memory");

    StoreHostResult(ctx, base, hidden_pointer, v);
  }
  if (thread_state && thread_state->IsStopRequested()) {
    throw rex::ThreadStop{};
  }
}

/** Bytes under r1 a leaf guest function may use without moving r1. */
inline constexpr u32 kRedZone = 1344;

/** Minimum frame of a guest function that calls others, 10 double words. */
inline constexpr u32 kLinkageSize = 0x50;

/** Return address a call from host code leaves in lr. */
inline constexpr u32 kHostReturnAddress = 0xBCBCBCBC;

/**
 * Reads stack_end_ptr from the KPCR at r13 + 0x74.
 *
 * @param ctx The register file.
 * @param base The guest memory base.
 * @return The stack limit, 0 when r13 is not set as in unit tests without a live thread.
 */
inline u32 GetStackLimit(PPCContext& ctx, u8* base) {
  if (ctx.r13.u32 == 0)
    return 0;
  u32 raw;
  std::memcpy(&raw, rex::runtime::GuestPtr(base, ctx.r13.u32 + 0x74), sizeof(raw));
  return std::byteswap(raw);
}

/**
 * Aborts when lowering r1 to a new value leaves the thread stack it is on.
 *
 * @param ctx The register file, r1 still at its current value.
 * @param base The guest memory base.
 * @param new_r1 The r1 the caller is about to set.
 */
inline void stack_bounds_check(PPCContext& ctx, u8* base, u32 new_r1) {
  const u32 limit = GetStackLimit(ctx, base);
  if (limit == 0) {
    return;
  }
  const u32 stack_base = *rex::runtime::GuestPtr<be_u32*>(base, ctx.r13.u32 + 0x70);
  if (ctx.r1.u32 > limit && ctx.r1.u32 <= stack_base) {
    REX_CHECK(cpu, new_r1 > limit, "PPC stack overflow, r1 ({:#x}) below stack_end_ptr ({:#x})",
              new_r1, limit);
  }
}

/**
 * Returns a 16 byte aligned r1 a given number of bytes under the current one.
 *
 * Aborts when the frame would wrap below address 0 or cross the thread's stack limit.
 *
 * @param ctx The register file.
 * @param base The guest memory base, null when no thread state is bound.
 * @param size The bytes the frame needs.
 * @return The new r1.
 */
inline u32 FrameBelow(PPCContext& ctx, u8* base, u32 size) {
  const u32 r1 = ctx.r1.u32;
  REX_CHECK(cpu, size <= r1, "PPC stack underflow, r1 ({:#x}) below a {:#x} byte frame", r1, size);
  const u32 new_r1 = (r1 - size) & ~0xFu;
  if (base) {
    stack_bounds_check(ctx, base, new_r1);
  }
  return new_r1;
}

/**
 * Opens a call frame under r1 for a call from host code into recompiled code.
 *
 * @param ctx The register file.
 * @param base The guest memory base.
 * @param stack_bytes The stack argument bytes the call needs.
 * @return The caller's r1.
 */
inline u32 OpenCallFrame(PPCContext& ctx, u8* base, u32 stack_bytes) {
  const u32 caller_r1 = ctx.r1.u32;
  ctx.r1.u64 = FrameBelow(ctx, base, kLinkageSize + stack_bytes);
  *rex::runtime::GuestPtr<be_u32*>(base, ctx.r1.u32) = caller_r1;
  ctx.lr = kHostReturnAddress;
  return caller_r1;
}

/**
 * Returns the stack argument bytes a call with these argument types needs.
 *
 * @tparam Args The argument types.
 * @return 8 bytes per argument slot past slot 8.
 */
template <typename... Args>
constexpr u32 GetStackArgumentBytes() {
  u32 end = 0;
  for (const ArgPlace& place : PlaceArgs<Args...>()) {
    end = std::max(end, place.slot + place.slots);
  }
  return end > 8 ? (end - 8) * 8 : 0;
}

/**
 * Returns the call frame bytes the result of a hidden return call needs.
 *
 * @tparam R The return type.
 * @return The structure size rounded up to 16 bytes, 0 when R is not returned through a pointer.
 */
template <typename R>
constexpr u32 HiddenReturnBytes() {
  if constexpr (HiddenReturnType<R>) {
    return (static_cast<u32>(kByValueSize<R>) + 15) & ~15u;
  } else {
    return 0;
  }
}

/**
 * Reads the result a guest function left in its result registers.
 *
 * @tparam R The result type.
 * @param base The guest memory base.
 * @param r3 The integer result register.
 * @param f1 The floating point result register.
 * @param v1 The vector result register.
 * @param result_storage The guest address of the hidden return storage, used by a hidden return.
 * @return The result.
 */
template <typename R>
R LoadGuestResult([[maybe_unused]] u8* base, [[maybe_unused]] const PPCRegister& r3,
                  [[maybe_unused]] const PPCRegister& f1, [[maybe_unused]] const PPCVRegister& v1,
                  [[maybe_unused]] u32 result_storage) {
  if constexpr (std::is_void_v<R>) {
    return;
  } else if constexpr (std::is_pointer_v<R>) {
    return r3.u32 ? rex::runtime::GuestPtr<R>(base, r3.u32) : nullptr;
  } else if constexpr (is_float_v<R>) {
    return static_cast<R>(f1.f64);
  } else if constexpr (VectorType<R>) {
    return v1;
  } else if constexpr (HiddenReturnType<R>) {
    return LoadByValue<R>(base, result_storage);
  } else if constexpr (ByValueType<R>) {
    return ByValueFromWord<R>(r3.u64);
  } else {
    return static_cast<R>(r3.u64);
  }
}

/**
 * Calls recompiled code on the thread's register file by the guest calling convention.
 *
 * @tparam R The return type, read from r3 or f1.
 * @tparam Args The argument types.
 * @param ctx The register file of the calling thread.
 * @param base The guest memory base.
 * @param fn The guest function.
 * @param args The arguments.
 * @return The value the guest function returned.
 */
template <typename R, typename... Args>
R Call(PPCContext& ctx, u8* base, PPCFunc& fn, Args... args) {
  const PPCContext saved = ctx;
  constexpr bool kHidden = HiddenReturnType<R>;
  u32 result_storage = 0;
  auto tpl = [&] {
    if constexpr (kHidden) {
      return std::make_tuple(u32{0}, args...);
    } else {
      return std::make_tuple(args...);
    }
  }();
  constexpr u32 kStackBytes =
      ((kHidden ? GetStackArgumentBytes<u32, Args...>() : GetStackArgumentBytes<Args...>()) + 15) &
      ~15u;
  constexpr u32 kStorageBytes = HiddenReturnBytes<R>();
  OpenCallFrame(ctx, base, kStackBytes + kStorageBytes);
  if constexpr (kHidden) {
    result_storage = ctx.r1.u32 + kLinkageSize + kStackBytes;
    std::get<0>(tpl) = result_storage;
  }
  TranslateArgsToGuest(ctx, base, tpl);
  fn(ctx, base);
  const PPCRegister r3 = ctx.r3;
  const PPCRegister f1 = ctx.f1;
  const PPCVRegister v1 = ctx.v1;
  const PPCFPSCRRegister fpscr = ctx.fpscr;
  ctx = saved;
  ctx.fpscr = fpscr;
  return LoadGuestResult<R>(base, r3, f1, v1, result_storage);
}

/**
 * Calls recompiled code on the calling thread's register file.
 *
 * @tparam T The return type.
 * @tparam TArgs The argument types, deduced from argv.
 * @param func The guest function.
 * @param argv The arguments.
 * @return The value the guest function returned.
 */
template <typename T, typename... TArgs>
T HostToGuestCall(PPCFunc* func, TArgs&&... argv) {
  auto* ts = rex::runtime::ThreadState::Get();
  REX_ASSERT(ts && func, "HostToGuestCall needs a bound thread state and a function");
  return Call<T>(*ts->GetContext(), ts->GetMemory()->GetVirtualMembase(), *func,
                 std::forward<TArgs>(argv)...);
}

}  // namespace rex::ppc

/** Size of the loaded image name buffer, 255 characters plus NUL. */
constexpr size_t kExLoadedImageNameSize = 255 + 1;
