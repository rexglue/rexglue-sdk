/**
 * @file        ppc/midasm.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

/**
 * Counts the arguments, up to 32.
 *
 * @code
 * REX_DETAIL_MIDASM_COUNT(a, b, c)
 * @endcode
 */
#define REX_DETAIL_MIDASM_COUNT(...)                                                             \
  REX_DETAIL_MIDASM_PICK(__VA_ARGS__ __VA_OPT__(, ) 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22,  \
                         21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, \
                         1, 0)

/**
 * Picks the count out of the arguments of REX_DETAIL_MIDASM_COUNT.
 *
 * @param ... Arguments whose 33rd entry is the count.
 *
 * @code
 * REX_DETAIL_MIDASM_PICK(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
 *                        22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 7, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, \
                               _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28,  \
                               _29, _30, _31, _32, count, ...)                                   \
  count

/**
 * Removes the parentheses around a parameter list.
 *
 * @code
 * REX_DETAIL_MIDASM_UNPAREN (u32 a, u32 b)
 * @endcode
 */
#define REX_DETAIL_MIDASM_UNPAREN(...) __VA_ARGS__

/**
 * Pastes two tokens after expanding them.
 *
 * @code
 * REX_DETAIL_MIDASM_EXPAND_PASTE(REX_DETAIL_MIDASM_LIST_, 3)
 * @endcode
 */
#define REX_DETAIL_MIDASM_EXPAND_PASTE(a, b) REX_DETAIL_MIDASM_PASTE(a, b)

/**
 * Pastes two tokens.
 *
 * @code
 * REX_DETAIL_MIDASM_PASTE(rex_arg, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_PASTE(a, b) a##b

/**
 * Declares the entry parameter of a body.
 *
 * @code
 * REX_DETAIL_MIDASM_PARAM(ExampleStep_rex_body, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_PARAM(body, index)                                           \
  rex::ppc::detail::MidAsmErasedT<rex::ppc::detail::MidAsmArg<decltype(&body), index>> \
      rex_arg##index

/**
 * Restores the entry argument of a body to its parameter type.
 *
 * @code
 * REX_DETAIL_MIDASM_ARG(ExampleStep_rex_body, 0)
 * @endcode
 */
#define REX_DETAIL_MIDASM_ARG(body, index)                                                 \
  rex::ppc::detail::RestoreMidAsmArg<rex::ppc::detail::MidAsmArg<decltype(&body), index>>( \
      rex_arg##index)

/**
 * Lists no parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_0(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_0(body, each)

/**
 * Lists one parameter.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_1(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_1(body, each) each(body, 0)

/**
 * Lists two parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_2(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_2(body, each) REX_DETAIL_MIDASM_LIST_1(body, each), each(body, 1)

/**
 * Lists three parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_3(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_3(body, each) REX_DETAIL_MIDASM_LIST_2(body, each), each(body, 2)

/**
 * Lists four parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_4(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_4(body, each) REX_DETAIL_MIDASM_LIST_3(body, each), each(body, 3)

/**
 * Lists five parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_5(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_5(body, each) REX_DETAIL_MIDASM_LIST_4(body, each), each(body, 4)

/**
 * Lists six parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_6(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_6(body, each) REX_DETAIL_MIDASM_LIST_5(body, each), each(body, 5)

/**
 * Lists seven parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_7(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_7(body, each) REX_DETAIL_MIDASM_LIST_6(body, each), each(body, 6)

/**
 * Lists eight parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_8(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_8(body, each) REX_DETAIL_MIDASM_LIST_7(body, each), each(body, 7)

/**
 * Lists nine parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_9(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_9(body, each) REX_DETAIL_MIDASM_LIST_8(body, each), each(body, 8)

/**
 * Lists ten parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_10(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_10(body, each) REX_DETAIL_MIDASM_LIST_9(body, each), each(body, 9)

/**
 * Lists 11 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_11(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_11(body, each) REX_DETAIL_MIDASM_LIST_10(body, each), each(body, 10)

/**
 * Lists 12 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_12(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_12(body, each) REX_DETAIL_MIDASM_LIST_11(body, each), each(body, 11)

/**
 * Lists 13 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_13(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_13(body, each) REX_DETAIL_MIDASM_LIST_12(body, each), each(body, 12)

/**
 * Lists 14 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_14(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_14(body, each) REX_DETAIL_MIDASM_LIST_13(body, each), each(body, 13)

/**
 * Lists 15 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_15(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_15(body, each) REX_DETAIL_MIDASM_LIST_14(body, each), each(body, 14)

/**
 * Lists 16 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_16(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_16(body, each) REX_DETAIL_MIDASM_LIST_15(body, each), each(body, 15)

/**
 * Lists 17 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_17(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_17(body, each) REX_DETAIL_MIDASM_LIST_16(body, each), each(body, 16)

/**
 * Lists 18 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_18(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_18(body, each) REX_DETAIL_MIDASM_LIST_17(body, each), each(body, 17)

/**
 * Lists 19 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_19(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_19(body, each) REX_DETAIL_MIDASM_LIST_18(body, each), each(body, 18)

/**
 * Lists 20 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_20(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_20(body, each) REX_DETAIL_MIDASM_LIST_19(body, each), each(body, 19)

/**
 * Lists 21 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_21(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_21(body, each) REX_DETAIL_MIDASM_LIST_20(body, each), each(body, 20)

/**
 * Lists 22 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_22(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_22(body, each) REX_DETAIL_MIDASM_LIST_21(body, each), each(body, 21)

/**
 * Lists 23 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_23(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_23(body, each) REX_DETAIL_MIDASM_LIST_22(body, each), each(body, 22)

/**
 * Lists 24 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_24(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_24(body, each) REX_DETAIL_MIDASM_LIST_23(body, each), each(body, 23)

/**
 * Lists 25 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_25(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_25(body, each) REX_DETAIL_MIDASM_LIST_24(body, each), each(body, 24)

/**
 * Lists 26 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_26(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_26(body, each) REX_DETAIL_MIDASM_LIST_25(body, each), each(body, 25)

/**
 * Lists 27 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_27(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_27(body, each) REX_DETAIL_MIDASM_LIST_26(body, each), each(body, 26)

/**
 * Lists 28 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_28(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_28(body, each) REX_DETAIL_MIDASM_LIST_27(body, each), each(body, 27)

/**
 * Lists 29 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_29(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_29(body, each) REX_DETAIL_MIDASM_LIST_28(body, each), each(body, 28)

/**
 * Lists 30 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_30(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_30(body, each) REX_DETAIL_MIDASM_LIST_29(body, each), each(body, 29)

/**
 * Lists 31 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_31(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_31(body, each) REX_DETAIL_MIDASM_LIST_30(body, each), each(body, 30)

/**
 * Lists 32 parameters.
 *
 * @param each The macro applied to the body and each parameter index.
 *
 * @code
 * REX_DETAIL_MIDASM_LIST_32(ExampleStep_rex_body, REX_DETAIL_MIDASM_PARAM)
 * @endcode
 */
#define REX_DETAIL_MIDASM_LIST_32(body, each) REX_DETAIL_MIDASM_LIST_31(body, each), each(body, 31)

/**
 * Defines the extern C entry of a body, which takes the erased parameter types.
 *
 * @param ret The return type, void for none.
 * @param name Name of the body and of its linked symbol.
 * @param params The parameter list in parentheses.
 *
 * @code
 * REX_DETAIL_MIDASM_ENTRY(void, ExampleStep, (PPCRegister& step))
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): ret declares a function
#define REX_DETAIL_MIDASM_ENTRY(ret, name, params)                                           \
  extern "C" ret name(REX_DETAIL_MIDASM_EXPAND_PASTE(                                        \
      REX_DETAIL_MIDASM_LIST_, REX_DETAIL_MIDASM_COUNT(REX_DETAIL_MIDASM_UNPAREN params))(   \
      name##_rex_body, REX_DETAIL_MIDASM_PARAM)) {                                           \
    return name##_rex_body(REX_DETAIL_MIDASM_EXPAND_PASTE(                                   \
        REX_DETAIL_MIDASM_LIST_, REX_DETAIL_MIDASM_COUNT(REX_DETAIL_MIDASM_UNPAREN params))( \
        name##_rex_body, REX_DETAIL_MIDASM_ARG));                                            \
  }
// NOLINTEND(bugprone-macro-parentheses)

namespace rex::ppc::detail {

enum class MidAsmType : u8 {
  kHook,     /**< Returns void, the generated code continues. */
  kBranch,   /**< Returns bool, the generated code jumps to the target when true. */
  kReturnIf, /**< Returns bool, the generated code returns from the function when true. */
};

enum class MidAsmParam : u8 {
  kInvalid, /**< A parameter type no register can fill. */
  kU32,     /**< u32 by value, read from an integer register. */
  kU64,     /**< u64 by value, read from an integer register. */
  kF64,     /**< f64 by value, read from a float register. */
  kVector,
  kVectorRef,
  kPointer,       /**< A pointer to an object, the guest address in an integer register. */
  kMappedPointer, /**< MappedPtr by value, the guest address in an integer register. */
  kRegister,
  kCR,
  kXER,
  kFPSCR,
  kContext,
};

enum class MidAsmRegisterType : u8 {
  kInvalid, /**< Not a register token, or r1. */
  kGPR,     /**< r0 to r31 except r1. */
  kFPR,     /**< f0 to f31. */
  kVR,      /**< v0 to v127. */
  kCR,      /**< Covers cr0 to cr7. */
  kCTR,
  kXER,
  kFPSCR,
  kReserved,
  kContext,
};

enum class MidAsmAtResult : u8 {
  kOk,
  kBadPlacement,     /**< The placement is neither before nor after. */
  kUnexpectedTarget, /**< A body that cannot branch was given a target. */
  kMissingTarget,
  kBadTarget,   /**< The target is not a nonzero multiple of 4. */
  kBadRegister, /**< A token is not a register, or is r1. */
  kCountMismatch,
  kTypeMismatch,
};

inline constexpr size_t kMaxMidAsmParams = 32;

struct MidAsmBody {
  MidAsmType type;
  size_t count;
  std::array<MidAsmParam, kMaxMidAsmParams> params;
};

/** Returns how a body takes a parameter of type T, kInvalid when no register fits. */
template <typename T>
constexpr MidAsmParam ClassifyMidAsmParam() {
  if constexpr (!std::is_reference_v<T>) {
    if constexpr (std::is_same_v<T, u32>) {
      return MidAsmParam::kU32;
    } else if constexpr (std::is_same_v<T, u64>) {
      return MidAsmParam::kU64;
    } else if constexpr (std::is_same_v<T, f64>) {
      return MidAsmParam::kF64;
    } else if constexpr (std::is_same_v<T, PPCVRegister>) {
      return MidAsmParam::kVector;
    } else if constexpr (std::is_pointer_v<T> && !std::is_function_v<std::remove_pointer_t<T>>) {
      return MidAsmParam::kPointer;
    } else if constexpr (memory::is_mapped_ptr_v<T>) {
      return MidAsmParam::kMappedPointer;
    } else {
      return MidAsmParam::kInvalid;
    }
  } else if constexpr (std::is_lvalue_reference_v<T> &&
                       !std::is_const_v<std::remove_reference_t<T>>) {
    using Bare = std::remove_reference_t<T>;
    if constexpr (std::is_same_v<Bare, PPCRegister>) {
      return MidAsmParam::kRegister;
    } else if constexpr (std::is_same_v<Bare, PPCVRegister>) {
      return MidAsmParam::kVectorRef;
    } else if constexpr (std::is_same_v<Bare, PPCCRRegister>) {
      return MidAsmParam::kCR;
    } else if constexpr (std::is_same_v<Bare, PPCXERRegister>) {
      return MidAsmParam::kXER;
    } else if constexpr (std::is_same_v<Bare, PPCFPSCRRegister>) {
      return MidAsmParam::kFPSCR;
    } else if constexpr (std::is_same_v<Bare, PPCContext>) {
      return MidAsmParam::kContext;
    } else {
      return MidAsmParam::kInvalid;
    }
  } else {
    return MidAsmParam::kInvalid;
  }
}

/** Reads the parameter types out of the type of a body. */
template <typename F>
struct MidAsmParams;

template <typename R, typename... A>
struct MidAsmParams<R (*)(A...)> {
  static constexpr size_t kCount = sizeof...(A);
  using Types = std::tuple<A...>;
  static constexpr std::array<MidAsmParam, sizeof...(A)> kTypes = {ClassifyMidAsmParam<A>()...};
};

template <typename F, size_t I>
using MidAsmArg = std::tuple_element_t<I, typename MidAsmParams<F>::Types>;

/**
 * The type a parameter takes in the extern C entry of a body. Object pointers become void* and
 * MappedPtr becomes MappedPtr<u8>.
 */
template <typename T>
struct MidAsmErased {
  using Type = T;
};

/** The entry type of an object pointer. */
template <typename T>
struct MidAsmErased<T*> {
  using Type = void*;
};

/** The entry type of a mapped pointer. */
template <typename T>
struct MidAsmErased<memory::MappedPtr<T>> {
  using Type = memory::MappedPtr<u8>;
};

/** The entry type of a parameter. */
template <typename T>
using MidAsmErasedT = typename MidAsmErased<T>::Type;

/** Converts an entry argument back to the parameter type of the body. */
template <typename T>
constexpr T RestoreMidAsmArg(MidAsmErasedT<T> value) {
  if constexpr (std::is_pointer_v<T>) {
    return static_cast<T>(value);
  } else if constexpr (memory::is_mapped_ptr_v<T>) {
    return T(reinterpret_cast<typename memory::MappedPtrInnerType<T>::type*>(value.GetHostPtr()),
             value.GetGuestAddress());
  } else {
    return value;
  }
}

/**
 * Builds the descriptor of a body from its type.
 *
 * @param type What the body does with the code around it.
 */
template <typename F>
constexpr MidAsmBody MakeMidAsmBody(MidAsmType type) {
  using Traits = MidAsmParams<F>;
  static_assert(Traits::kCount <= kMaxMidAsmParams, "A mid asm body takes at most 32 parameters");
  MidAsmBody body{type, Traits::kCount, {}};
  for (size_t index = 0; index < Traits::kCount; ++index) {
    body.params[index] = Traits::kTypes[index];
  }
  return body;
}

/**
 * Reports whether every parameter of a body can take a register.
 *
 * @return True when no parameter is kInvalid.
 */
constexpr bool IsMidAsmBodyValid(const MidAsmBody& body) {
  for (size_t index = 0; index < body.count; ++index) {
    if (body.params[index] == MidAsmParam::kInvalid) {
      return false;
    }
  }
  return true;
}

/**
 * Parses a decimal number written without a sign or leading zeros.
 *
 * @param limit The largest accepted value.
 * @param value Receives the number.
 * @return True when the text is a canonical number no larger than limit.
 */
constexpr bool ParseMidAsmIndex(std::string_view text, u32 limit, u32& value) {
  if (text.empty() || text.size() > 3 || (text.size() > 1 && text[0] == '0')) {
    return false;
  }
  u32 result = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    result = result * 10 + static_cast<u32>(digit - '0');
  }
  if (result > limit) {
    return false;
  }
  value = result;
  return true;
}

/**
 * Parses an address written as a C++ integer literal, with ' separators and u or l suffixes.
 *
 * @param value Receives the number.
 * @return True when the text is a literal that fits 32 bits.
 */
constexpr bool ParseMidAsmAddress(std::string_view text, u32& value) {
  while (!text.empty() &&
         (text.back() == 'u' || text.back() == 'U' || text.back() == 'l' || text.back() == 'L')) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return false;
  }
  u64 result = 0;
  size_t index = 0;
  u32 radix = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    radix = 16;
    index = 2;
  } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
    radix = 2;
    index = 2;
  } else if (text.size() > 1 && text[0] == '0') {
    radix = 8;
    index = 1;
  }
  bool digits = false;
  for (; index < text.size(); ++index) {
    const char digit = text[index];
    u32 digit_value = 0;
    if (digit == '\'' && digits && index + 1 < text.size()) {
      continue;
    }
    if (digit >= '0' && digit <= '9') {
      digit_value = static_cast<u32>(digit - '0');
    } else if (radix == 16 && digit >= 'a' && digit <= 'f') {
      digit_value = static_cast<u32>(digit - 'a') + 10;
    } else if (radix == 16 && digit >= 'A' && digit <= 'F') {
      digit_value = static_cast<u32>(digit - 'A') + 10;
    } else {
      return false;
    }
    if (digit_value >= radix) {
      return false;
    }
    digits = true;
    result = result * radix + digit_value;
    if (result > 0xFFFFFFFFull) {
      return false;
    }
  }
  if (!digits) {
    return false;
  }
  value = static_cast<u32>(result);
  return true;
}

/**
 * Reports whether a value is usable as an instruction address.
 *
 * @return True when the value is nonzero, a multiple of 4 and fits 32 bits.
 */
constexpr bool IsMidAsmAddress(u64 address) {
  return address != 0 && address % 4 == 0 && address <= 0xFFFFFFFFull;
}

constexpr std::string_view TrimMidAsmToken(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                           text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                           text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

/**
 * Classifies a register token.
 *
 * @param token The token as written, without blanks.
 * @return The register file the token names, kInvalid for anything else and for r1.
 */
constexpr MidAsmRegisterType ParseMidAsmRegister(std::string_view token) {
  u32 index = 0;
  if (token == "ctx") {
    return MidAsmRegisterType::kContext;
  }
  if (token == "ctr") {
    return MidAsmRegisterType::kCTR;
  }
  if (token == "xer") {
    return MidAsmRegisterType::kXER;
  }
  if (token == "fpscr") {
    return MidAsmRegisterType::kFPSCR;
  }
  if (token == "reserved") {
    return MidAsmRegisterType::kReserved;
  }
  if (token.starts_with("cr")) {
    return ParseMidAsmIndex(token.substr(2), 7, index) ? MidAsmRegisterType::kCR
                                                       : MidAsmRegisterType::kInvalid;
  }
  if (token.starts_with("r")) {
    return ParseMidAsmIndex(token.substr(1), 31, index) && index != 1
               ? MidAsmRegisterType::kGPR
               : MidAsmRegisterType::kInvalid;
  }
  if (token.starts_with("f")) {
    return ParseMidAsmIndex(token.substr(1), 31, index) ? MidAsmRegisterType::kFPR
                                                        : MidAsmRegisterType::kInvalid;
  }
  if (token.starts_with("v")) {
    return ParseMidAsmIndex(token.substr(1), 127, index) ? MidAsmRegisterType::kVR
                                                         : MidAsmRegisterType::kInvalid;
  }
  return MidAsmRegisterType::kInvalid;
}

/**
 * Reports whether a register can fill a parameter.
 *
 * @param param How the body takes the register.
 * @return True when the body can be called with the register.
 */
constexpr bool IsMidAsmRegisterFit(MidAsmRegisterType reg, MidAsmParam param) {
  switch (reg) {
    case MidAsmRegisterType::kGPR:
      return param == MidAsmParam::kU32 || param == MidAsmParam::kU64 ||
             param == MidAsmParam::kRegister || param == MidAsmParam::kPointer ||
             param == MidAsmParam::kMappedPointer;
    case MidAsmRegisterType::kFPR:
      return param == MidAsmParam::kF64 || param == MidAsmParam::kRegister;
    case MidAsmRegisterType::kVR:
      return param == MidAsmParam::kVector || param == MidAsmParam::kVectorRef;
    case MidAsmRegisterType::kCR:
      return param == MidAsmParam::kCR;
    case MidAsmRegisterType::kCTR:
    case MidAsmRegisterType::kReserved:
      return param == MidAsmParam::kRegister;
    case MidAsmRegisterType::kXER:
      return param == MidAsmParam::kXER;
    case MidAsmRegisterType::kFPSCR:
      return param == MidAsmParam::kFPSCR;
    case MidAsmRegisterType::kContext:
      return param == MidAsmParam::kContext;
    case MidAsmRegisterType::kInvalid:
      return false;
  }
  return false;
}

/**
 * Checks how a REX_MIDASM_AT line attaches a body.
 *
 * @param placement The placement token as written.
 * @param arguments The arguments after the placement as written, comma separated, target first.
 * @return kOk when the line fits the body, else the first problem found.
 */
constexpr MidAsmAtResult CheckMidAsmAt(const MidAsmBody& body, std::string_view placement,
                                       std::string_view arguments) {
  if (TrimMidAsmToken(placement) != "before" && TrimMidAsmToken(placement) != "after") {
    return MidAsmAtResult::kBadPlacement;
  }
  std::string_view rest = TrimMidAsmToken(arguments);
  const bool has_arguments = !rest.empty();
  size_t registers = 0;
  bool first = true;
  while (has_arguments) {
    const size_t comma = rest.find(',');
    const std::string_view token = TrimMidAsmToken(rest.substr(0, comma));
    const bool is_number = !token.empty() && token[0] >= '0' && token[0] <= '9';
    if (first && body.type == MidAsmType::kBranch) {
      if (!is_number) {
        return MidAsmAtResult::kMissingTarget;
      }
      u32 target = 0;
      if (!ParseMidAsmAddress(token, target) || !IsMidAsmAddress(target)) {
        return MidAsmAtResult::kBadTarget;
      }
    } else if (first && is_number) {
      return MidAsmAtResult::kUnexpectedTarget;
    } else {
      const MidAsmRegisterType type = ParseMidAsmRegister(token);
      if (type == MidAsmRegisterType::kInvalid) {
        return MidAsmAtResult::kBadRegister;
      }
      if (registers < body.count && !IsMidAsmRegisterFit(type, body.params[registers])) {
        return MidAsmAtResult::kTypeMismatch;
      }
      ++registers;
    }
    first = false;
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  if (body.type == MidAsmType::kBranch && first) {
    return MidAsmAtResult::kMissingTarget;
  }
  if (registers != body.count) {
    return MidAsmAtResult::kCountMismatch;
  }
  return MidAsmAtResult::kOk;
}

}  // namespace rex::ppc::detail

/**
 * Declares a mid asm body and its descriptor, defines the extern C entry the generated code calls,
 * then opens the body definition.
 *
 * @param type The MidAsmType enumerator.
 * @param ret The return type, void for none.
 * @param name Name of the body and of its linked symbol.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_DETAIL_MIDASM_BODY(kHook, void, ExampleStep, (PPCRegister& step)) {
 *   step.f64 *= 2.0;
 * }
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): ret and params declare a function
#define REX_DETAIL_MIDASM_BODY(type, ret, name, params)                                      \
  static ret name##_rex_body params;                                                         \
  inline constexpr rex::ppc::detail::MidAsmBody name##_rex_midasm =                          \
      rex::ppc::detail::MakeMidAsmBody<decltype(&name##_rex_body)>(                          \
          rex::ppc::detail::MidAsmType::type);                                               \
  static_assert(rex::ppc::detail::IsMidAsmBodyValid(name##_rex_midasm),                      \
                "A mid asm body takes u32, u64, f64, PPCVRegister, a pointer, a MappedPtr, " \
                "or a non const reference to a register type or PPCContext");                \
  REX_DETAIL_MIDASM_ENTRY(ret, name, params)                                                 \
  static ret name##_rex_body params
// NOLINTEND(bugprone-macro-parentheses)

/**
 * Declares a mid asm body that returns void. A pointer or MappedPtr parameter takes an integer
 * register holding a guest address and receives the host pointer, null for 0.
 *
 * @param name Name of the body and of its linked symbol.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_MIDASM_HOOK(ExampleStep, (PPCRegister& step)) {
 *   step.f64 *= 2.0;
 * }
 * REX_MIDASM_AT(ExampleStep, 0x82000100, after, f10);
 * @endcode
 */
#define REX_MIDASM_HOOK(name, params) REX_DETAIL_MIDASM_BODY(kHook, void, name, params)

/**
 * Declares a mid asm body that returns bool and makes the generated code jump to the target of
 * its REX_MIDASM_AT line when it returns true.
 *
 * @param name Name of the body and of its linked symbol.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_MIDASM_BRANCH(ExampleSkip, (u32 size)) {
 *   return size == 0;
 * }
 * REX_MIDASM_AT(ExampleSkip, 0x82000300, before, 0x8200030C, r5);
 * @endcode
 */
#define REX_MIDASM_BRANCH(name, params) REX_DETAIL_MIDASM_BODY(kBranch, bool, name, params)

/**
 * Declares a mid asm body that returns bool and makes the generated code return from the function
 * when it returns true.
 *
 * @param name Name of the body and of its linked symbol.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_MIDASM_RETURN_IF(ExampleSkipAll, (u32 count)) {
 *   return count == 0;
 * }
 * REX_MIDASM_AT(ExampleSkipAll, 0x82000400, before, r3);
 * @endcode
 */
#define REX_MIDASM_RETURN_IF(name, params) REX_DETAIL_MIDASM_BODY(kReturnIf, bool, name, params)

/**
 * Fails the build when a REX_MIDASM_AT line does not fit its body.
 *
 * @param placement The placement token, stringified.
 * @param arguments The arguments after the placement, stringified.
 * @param code The MidAsmAtResult enumerator that fails the build.
 * @param text The message.
 *
 * @code
 * REX_DETAIL_MIDASM_AT_CHECK(ExampleStep, "after", "f10", kBadPlacement, "bad placement");
 * @endcode
 */
#define REX_DETAIL_MIDASM_AT_CHECK(name, placement, arguments, code, text)                  \
  static_assert(rex::ppc::detail::CheckMidAsmAt(name##_rex_midasm, placement, arguments) != \
                    rex::ppc::detail::MidAsmAtResult::code,                                 \
                text)

/**
 * Attaches a mid asm body at an instruction address. A branch body takes its target first, then
 * every body takes one register token per parameter, ctx for a PPCContext parameter. A line that
 * does not fit its body fails to compile.
 *
 * @param name Name of the body.
 * @param address The instruction address, a literal.
 * @param placement The token before or after.
 * @param ... The branch target for a branch body, then the register tokens.
 *
 * @code
 * REX_MIDASM_AT(ExampleStep, 0x82000100, after, f10);
 * @endcode
 */
#define REX_MIDASM_AT(name, address, placement, ...)                                           \
  static_assert(rex::ppc::detail::IsMidAsmAddress(address),                                    \
                "REX_MIDASM_AT address must be a nonzero multiple of 4");                      \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kBadPlacement,                    \
                             "REX_MIDASM_AT placement must be before or after");               \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kUnexpectedTarget,                \
                             "REX_MIDASM_AT takes a target only for a branch body");           \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kMissingTarget,                   \
                             "REX_MIDASM_AT needs the target of a branch body first");         \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kBadTarget,                       \
                             "REX_MIDASM_AT target must be a nonzero multiple of 4");          \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kBadRegister,                     \
                             "REX_MIDASM_AT register must be r0 to r31 except r1, f0 to f31, " \
                             "v0 to v127, cr0 to cr7, ctr, xer, fpscr, reserved or ctx");      \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kCountMismatch,                   \
                             "REX_MIDASM_AT register count must equal the body parameters");   \
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kTypeMismatch,                    \
                             "REX_MIDASM_AT register type does not fit its parameter")
