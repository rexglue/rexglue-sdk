/**
 * @file        ppc/midasm.h
 * @brief       Mid asm hook bodies declared in C++ and the checks on how they attach
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

#include <rex/ppc/context.h>
#include <rex/ppc/midasm_entry.h>
#include <rex/types.h>

namespace rex::ppc::detail {

/** What a mid asm body does with the generated code around the call. */
enum class MidAsmKind : u8 {
  kHook,     /**< Returns void, the generated code continues. */
  kBranch,   /**< Returns bool, the generated code jumps to the target when true. */
  kReturnIf, /**< Returns bool, the generated code returns from the function when true. */
};

/** How a mid asm body takes one register. */
enum class MidAsmParam : u8 {
  kInvalid,       /**< A parameter type no register can fill. */
  kU32,           /**< u32 by value, read from an integer register. */
  kU64,           /**< u64 by value, read from an integer register. */
  kF64,           /**< f64 by value, read from a float register. */
  kVector,        /**< PPCVRegister by value. */
  kVectorRef,     /**< PPCVRegister by reference. */
  kPointer,       /**< A pointer to an object, the guest address in an integer register. */
  kMappedPointer, /**< MappedPtr by value, the guest address in an integer register. */
  kRegister,      /**< PPCRegister by reference. */
  kCR,            /**< PPCCRRegister by reference. */
  kXER,           /**< PPCXERRegister by reference. */
  kFPSCR,         /**< PPCFPSCRRegister by reference. */
  kContext,       /**< PPCContext by reference. */
};

/** The register file a register token names. */
enum class MidAsmRegisterKind : u8 {
  kInvalid,  /**< Not a register token, or r1. */
  kGPR,      /**< r0 to r31 except r1. */
  kFPR,      /**< f0 to f31. */
  kVR,       /**< v0 to v127. */
  kCR,       /**< cr0 to cr7. */
  kCTR,      /**< ctr. */
  kXER,      /**< xer. */
  kFPSCR,    /**< fpscr. */
  kReserved, /**< reserved. */
  kContext,  /**< ctx. */
};

/** The outcome of checking one REX_MIDASM_AT line against its body. */
enum class MidAsmAtResult : u8 {
  kOk,               /**< The line fits its body. */
  kBadPlacement,     /**< The placement is neither before nor after. */
  kUnexpectedTarget, /**< A body that cannot branch was given a target. */
  kMissingTarget,    /**< A branch body was given no target. */
  kBadTarget,        /**< The target is not a nonzero multiple of 4. */
  kBadRegister,      /**< A token is not a register, or is r1. */
  kCountMismatch,    /**< The registers and the parameters differ in number. */
  kKindMismatch,     /**< A register does not fit its parameter. */
};

/** The most parameters a mid asm body can take. */
inline constexpr size_t kMaxMidAsmParams = 32;

/** What a mid asm body declares, built from its signature by the body macros. */
struct MidAsmBody {
  MidAsmKind kind; /**< What the body does with the code around it. */
  size_t count;    /**< The number of parameters. */
  std::array<MidAsmParam, kMaxMidAsmParams> params; /**< How each parameter takes its register. */
};

/**
 * Returns how a body takes a parameter of type T.
 *
 * @tparam T The parameter type.
 * @return The parameter kind, kInvalid when no register fits.
 */
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
    } else if constexpr (is_mapped_ptr_v<T>) {
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

/**
 * Reads the parameter kinds out of the type of a body.
 *
 * @tparam F The body function pointer type.
 */
template <typename F>
struct MidAsmParams;

/**
 * The parameters of a body.
 *
 * @tparam R The return type.
 * @tparam A The parameter types.
 */
template <typename R, typename... A>
struct MidAsmParams<R (*)(A...)> {
  static constexpr size_t kCount = sizeof...(A); /**< The number of parameters. */
  using Types = std::tuple<A...>;                /**< The parameter types. */
  static constexpr std::array<MidAsmParam, sizeof...(A)> kKinds = {
      ClassifyMidAsmParam<A>()...}; /**< How each parameter takes its register. */
};

/**
 * The type of one parameter of a body.
 *
 * @tparam F The body function pointer type.
 * @tparam I The parameter index.
 */
template <typename F, size_t I>
using MidAsmArg = std::tuple_element_t<I, typename MidAsmParams<F>::Types>;

/**
 * The type a parameter takes in the extern C entry of a body. The generated code cannot name the
 * types of the title, so it declares every object pointer as void* and every MappedPtr as
 * MappedPtr<u8>.
 *
 * @tparam T The parameter type of the body.
 */
template <typename T>
struct MidAsmErased {
  using Type = T; /**< The parameter type of the entry. */
};

/**
 * The entry type of an object pointer.
 *
 * @tparam T The pointee type.
 */
template <typename T>
struct MidAsmErased<T*> {
  using Type = void*; /**< The parameter type of the entry. */
};

/**
 * The entry type of a mapped pointer.
 *
 * @tparam T The pointee type.
 */
template <typename T>
struct MidAsmErased<MappedPtr<T>> {
  using Type = MappedPtr<u8>; /**< The parameter type of the entry. */
};

/**
 * The entry type of a parameter.
 *
 * @tparam T The parameter type of the body.
 */
template <typename T>
using MidAsmErasedT = typename MidAsmErased<T>::Type;

/**
 * Converts an entry argument back to the parameter type of the body.
 *
 * @tparam T The parameter type of the body.
 * @param value The entry argument.
 * @return The argument as the body takes it.
 */
template <typename T>
constexpr T RestoreMidAsmArg(MidAsmErasedT<T> value) {
  if constexpr (std::is_pointer_v<T>) {
    return static_cast<T>(value);
  } else if constexpr (is_mapped_ptr_v<T>) {
    return T(reinterpret_cast<typename MappedPtrInnerType<T>::type*>(value.GetHostPtr()),
             value.GetGuestAddress());
  } else {
    return value;
  }
}

/**
 * Builds the descriptor of a body from its type.
 *
 * @tparam F The body function pointer type.
 * @param kind What the body does with the code around it.
 * @return The descriptor.
 */
template <typename F>
constexpr MidAsmBody MakeMidAsmBody(MidAsmKind kind) {
  using Traits = MidAsmParams<F>;
  static_assert(Traits::kCount <= kMaxMidAsmParams, "A mid asm body takes at most 32 parameters");
  MidAsmBody body{kind, Traits::kCount, {}};
  for (size_t index = 0; index < Traits::kCount; ++index) {
    body.params[index] = Traits::kKinds[index];
  }
  return body;
}

/**
 * Returns true when every parameter of a body can take a register.
 *
 * @param body The descriptor.
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
 * @param text The digits.
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
 * Parses an address written as a C++ integer literal, decimal, octal, 0b binary or 0x hexadecimal,
 * with ' digit separators and u or l suffixes.
 *
 * @param text The literal.
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
 * Returns true when a value is usable as an instruction address.
 *
 * @param address The value.
 * @return True when the value is nonzero, a multiple of 4 and fits 32 bits.
 */
constexpr bool IsMidAsmAddress(u64 address) {
  return address != 0 && address % 4 == 0 && address <= 0xFFFFFFFFull;
}

/**
 * Removes the blanks around a token.
 *
 * @param text The token.
 * @return The token without leading and trailing blanks.
 */
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
constexpr MidAsmRegisterKind ParseMidAsmRegister(std::string_view token) {
  u32 index = 0;
  if (token == "ctx") {
    return MidAsmRegisterKind::kContext;
  }
  if (token == "ctr") {
    return MidAsmRegisterKind::kCTR;
  }
  if (token == "xer") {
    return MidAsmRegisterKind::kXER;
  }
  if (token == "fpscr") {
    return MidAsmRegisterKind::kFPSCR;
  }
  if (token == "reserved") {
    return MidAsmRegisterKind::kReserved;
  }
  if (token.starts_with("cr")) {
    return ParseMidAsmIndex(token.substr(2), 7, index) ? MidAsmRegisterKind::kCR
                                                       : MidAsmRegisterKind::kInvalid;
  }
  if (token.starts_with("r")) {
    return ParseMidAsmIndex(token.substr(1), 31, index) && index != 1
               ? MidAsmRegisterKind::kGPR
               : MidAsmRegisterKind::kInvalid;
  }
  if (token.starts_with("f")) {
    return ParseMidAsmIndex(token.substr(1), 31, index) ? MidAsmRegisterKind::kFPR
                                                        : MidAsmRegisterKind::kInvalid;
  }
  if (token.starts_with("v")) {
    return ParseMidAsmIndex(token.substr(1), 127, index) ? MidAsmRegisterKind::kVR
                                                         : MidAsmRegisterKind::kInvalid;
  }
  return MidAsmRegisterKind::kInvalid;
}

/**
 * Returns true when a register can fill a parameter.
 *
 * @param reg The register file.
 * @param param How the body takes the register.
 * @return True when the body can be called with the register.
 */
constexpr bool IsMidAsmRegisterFit(MidAsmRegisterKind reg, MidAsmParam param) {
  switch (reg) {
    case MidAsmRegisterKind::kGPR:
      return param == MidAsmParam::kU32 || param == MidAsmParam::kU64 ||
             param == MidAsmParam::kRegister || param == MidAsmParam::kPointer ||
             param == MidAsmParam::kMappedPointer;
    case MidAsmRegisterKind::kFPR:
      return param == MidAsmParam::kF64 || param == MidAsmParam::kRegister;
    case MidAsmRegisterKind::kVR:
      return param == MidAsmParam::kVector || param == MidAsmParam::kVectorRef;
    case MidAsmRegisterKind::kCR:
      return param == MidAsmParam::kCR;
    case MidAsmRegisterKind::kCTR:
    case MidAsmRegisterKind::kReserved:
      return param == MidAsmParam::kRegister;
    case MidAsmRegisterKind::kXER:
      return param == MidAsmParam::kXER;
    case MidAsmRegisterKind::kFPSCR:
      return param == MidAsmParam::kFPSCR;
    case MidAsmRegisterKind::kContext:
      return param == MidAsmParam::kContext;
    case MidAsmRegisterKind::kInvalid:
      return false;
  }
  return false;
}

/**
 * Checks how a REX_MIDASM_AT line attaches a body.
 *
 * @param body The descriptor of the body.
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
    if (first && body.kind == MidAsmKind::kBranch) {
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
      const MidAsmRegisterKind kind = ParseMidAsmRegister(token);
      if (kind == MidAsmRegisterKind::kInvalid) {
        return MidAsmAtResult::kBadRegister;
      }
      if (registers < body.count && !IsMidAsmRegisterFit(kind, body.params[registers])) {
        return MidAsmAtResult::kKindMismatch;
      }
      ++registers;
    }
    first = false;
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  if (body.kind == MidAsmKind::kBranch && first) {
    return MidAsmAtResult::kMissingTarget;
  }
  if (registers != body.count) {
    return MidAsmAtResult::kCountMismatch;
  }
  return MidAsmAtResult::kOk;
}

}  // namespace rex::ppc::detail

/**
 * Declares a mid asm body and its descriptor, defines the extern C entry that the generated code
 * calls, then opens the definition of the body. The entry takes object pointers as void* and
 * MappedPtr values as MappedPtr<u8>, so the generated declaration matches it without the types of
 * the title. The macros that name a kind use it.
 *
 * @param kind The MidAsmKind enumerator.
 * @param ret The return type.
 * @param name The body name, also the linked name of its entry.
 * @param params The parameter list in parentheses, with names.
 *
 * @code
 * REX_DETAIL_MIDASM_BODY(kHook, void, ExampleStep, (PPCRegister& step)) {
 *   step.f64 *= 2.0;
 * }
 * @endcode
 */
// NOLINTBEGIN(bugprone-macro-parentheses): ret and params declare a function
#define REX_DETAIL_MIDASM_BODY(kind, ret, name, params)                                      \
  static ret name##_rex_body params;                                                         \
  inline constexpr rex::ppc::detail::MidAsmBody name##_rex_midasm =                          \
      rex::ppc::detail::MakeMidAsmBody<decltype(&name##_rex_body)>(                          \
          rex::ppc::detail::MidAsmKind::kind);                                               \
  static_assert(rex::ppc::detail::IsMidAsmBodyValid(name##_rex_midasm),                      \
                "A mid asm body takes u32, u64, f64, PPCVRegister, a pointer, a MappedPtr, " \
                "or a non const reference to a register type or PPCContext");                \
  REX_DETAIL_MIDASM_ENTRY(ret, name, params)                                                 \
  static ret name##_rex_body params
// NOLINTEND(bugprone-macro-parentheses)

/**
 * Declares a mid asm body that returns void and lets the generated code continue. The body is
 * defined right after the macro, and REX_MIDASM_AT lines in the same scope attach it. A pointer or
 * MappedPtr parameter takes an integer register, whose low word is a guest address, and receives
 * the host pointer to it, null for 0.
 *
 * @param name The body name, also its linked name.
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
 * @param name The body name, also its linked name.
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
 * @param name The body name, also its linked name.
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
 * @param name The body name.
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
 * Attaches a mid asm body at an instruction address. It follows the body macro in the same scope.
 * A branch body takes its target as the first argument after the placement. Every body then takes
 * one register token per parameter, in order. The tokens are r0 to r31 except r1, f0 to f31, v0 to
 * v127, cr0 to cr7, ctr, xer, fpscr and reserved, and ctx for a PPCContext parameter. A line that
 * does not fit its body fails to compile.
 *
 * @param name The body name.
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
  REX_DETAIL_MIDASM_AT_CHECK(name, #placement, #__VA_ARGS__, kKindMismatch,                    \
                             "REX_MIDASM_AT register kind does not fit its parameter")
