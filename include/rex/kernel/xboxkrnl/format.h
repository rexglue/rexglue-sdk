/**
 * @file        kernel/xboxkrnl/format.h
 * @brief       PPC printf format engine for kernel string exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#include <rex/assert.h>
#include <rex/memory/byte_swap.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/types.h>

namespace rex::runtime::detail {

/** States of the printf format specifier parser. */
enum FormatState : u8 {
  kFSInvalid = 0,    /**< Invalid state. */
  kFSUnknown,        /**< Copying literal text outside a specifier. */
  kFSStart,          /**< After the percent sign. */
  kFSFlags,          /**< Reading flag characters. */
  kFSWidth,          /**< Reading the field width. */
  kFSPrecisionStart, /**< Checking for a precision. */
  kFSPrecision,      /**< Reading the precision. */
  kFSSize,           /**< Reading the size prefix. */
  kFSType,           /**< Reading the conversion character. */
  kFSEnd,            /**< Specifier complete. */
};

/** Flags collected while parsing a printf format specifier. */
enum FormatFlags : u16 {
  kFFLeftJustify = 1 << 0,        /**< Left justify the field. */
  kFFAddLeadingZeros = 1 << 1,    /**< Pad the field with zeros. */
  kFFAddPositive = 1 << 2,        /**< Print a plus sign on positive values. */
  kFFAddPositiveAsSpace = 1 << 3, /**< Print a space on positive values. */
  kFFAddNegative = 1 << 4,        /**< The value is negative, so print a minus sign. */
  kFFAddPrefix = 1 << 5,          /**< Print the alternate form prefix. */
  kFFIsShort = 1 << 6,            /**< Short size prefix. */
  kFFIsLong = 1 << 7,             /**< Long size prefix. */
  kFFIsLongLong = 1 << 8,         /**< Long long size prefix. */
  kFFIsWide = 1 << 9,             /**< Wide size prefix. */
  kFFIsSigned = 1 << 10,          /**< Signed integer conversion. */
  kFFForceLeadingZero = 1 << 11,  /**< Octal conversion with the alternate form. */
  kFFInvertWide = 1 << 12,        /**< Flips the width of the format string character type. */
};

/** Size prefix of a printf conversion. */
enum ArgumentSize : u8 {
  kASDefault = 0, /**< No size prefix. */
  kASShort,       /**< Short size prefix. */
  kASLong,        /**< Long size prefix. */
  kASLongLong,    /**< Long long size prefix. */
};

/** Format string source that also collects the output. */
template <typename T>
concept FormatDataSource = requires(T& t, i32 n, u16 c) {
  { t.get() } -> std::same_as<u16>;
  { t.peek(n) } -> std::same_as<u16>;
  { t.skip(n) } -> std::same_as<void>;
  { t.put(c) } -> std::same_as<bool>;
};

/** Source of 32 bit and 64 bit variadic arguments. */
template <typename T>
concept ArgListSource = requires(T& t) {
  { t.get32() } -> std::same_as<u32>;
  { t.get64() } -> std::same_as<u64>;
};

/**
 * Formats a floating point value for the e, E, f, g, G, a and A conversions.
 *
 * @param value The value to format.
 * @param precision Digits to print, negative for the default of 6.
 * @param c The conversion character.
 * @param flags FormatFlags bits.
 * @return The formatted text.
 */
inline std::string FormatDouble(f64 value, i32 precision, u16 c, u32 flags) {
  if (precision < 0) {
    precision = 6;
  } else if (precision == 0 && c == 'g') {
    precision = 1;
  }

  std::ostringstream temp;
  temp << std::setprecision(precision);

  if (c == 'f') {
    temp << std::fixed;
  } else if (c == 'e' || c == 'E') {
    temp << std::scientific;
  } else if (c == 'a' || c == 'A') {
    temp << std::hexfloat;
  } else if (c == 'g' || c == 'G') {
    temp << std::defaultfloat;
  }

  if (c == 'E' || c == 'G' || c == 'A') {
    temp << std::uppercase;
  }

  if (flags & kFFAddPrefix) {
    temp << std::showpoint;
  }

  temp << value;
  return temp.str();
}

}  // namespace rex::runtime::detail

namespace rex::runtime {

/**
 * Reads variadic arguments from PPC registers and the stack, for sprintf, _snprintf, DbgPrint and
 * others where varargs follow fixed parameters. Arguments 0 to 7 come from r3 to r10 as 64 bit
 * values, the rest from 8 byte slots at r1 + 0x54.
 */
class StackArgList {
 public:
  /**
   * Constructs the list positioned at an argument index.
   *
   * @param ctx The PPC context holding r1 and r3 to r10.
   * @param base The host base of guest memory.
   * @param start_index Index of the first variadic argument, counting the fixed parameters.
   */
  StackArgList(PPCContext& ctx, u8* base, i32 start_index)
      : ctx_(ctx), base_(base), index_(start_index) {}

  u32 get32() { return static_cast<u32>(get64()); }

  /**
   * Returns the next argument and advances the index.
   *
   * @return The argument as a 64 bit value.
   */
  u64 get64() {
    u64 value;
    if (index_ <= 7) {
      switch (index_) {
        case 0:
          value = ctx_.r3.u64;
          break;
        case 1:
          value = ctx_.r4.u64;
          break;
        case 2:
          value = ctx_.r5.u64;
          break;
        case 3:
          value = ctx_.r6.u64;
          break;
        case 4:
          value = ctx_.r7.u64;
          break;
        case 5:
          value = ctx_.r8.u64;
          break;
        case 6:
          value = ctx_.r9.u64;
          break;
        case 7:
          value = ctx_.r10.u64;
          break;
        default:
          value = 0;
          break;
      }
    } else {
      u32 stack_addr = ctx_.r1.u32 + 0x54 + ((index_ - 8) * 8);
      value = static_cast<u64>(*rex::runtime::GuestPtr<be_u64*>(base_, stack_addr));
    }
    ++index_;
    return value;
  }

 private:
  PPCContext& ctx_;
  u8* base_;
  i32 index_;
};

/**
 * Reads arguments from a guest va_list, a pointer to an array of 8 byte aligned values. Used by
 * vsprintf, _vsnprintf and others where a va_list pointer is passed.
 */
class ArrayArgList {
 public:
  /**
   * Constructs the list over a va_list array.
   *
   * @param base The host base of guest memory.
   * @param arg_ptr Guest address of the va_list array.
   */
  ArrayArgList(u8* base, u32 arg_ptr) : base_(base), arg_ptr_(arg_ptr), index_(0) {}

  u32 get32() { return static_cast<u32>(get64()); }

  /**
   * Returns the next argument and advances the index.
   *
   * @return The argument as a 64 bit value.
   */
  u64 get64() {
    u32 addr = arg_ptr_ + (8 * index_);
    u64 value = static_cast<u64>(*rex::runtime::GuestPtr<be_u64*>(base_, addr));
    ++index_;
    return value;
  }

 private:
  u8* base_;
  u32 arg_ptr_;
  i32 index_;
};

/** Narrow char format string source and sink. */
class StringFormatData {
 public:
  /**
   * Constructs the source over a format string.
   *
   * @param input Host pointer to the NUL terminated narrow format string.
   */
  explicit StringFormatData(const u8* input) : input_(input) {}

  /**
   * Returns the next character and advances, or returns 0 at the end without advancing.
   *
   * @return The character.
   */
  u16 get() {
    u16 result = *input_;
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return input_[offset]; }

  /**
   * Advances past up to count characters, stopping at the end.
   *
   * @param count Number of characters to skip.
   */
  void skip(i32 count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  /**
   * Appends a character to the output.
   *
   * @param c The character.
   * @return True when appended, false when c does not fit in a narrow character.
   */
  bool put(u16 c) {
    if (c >= 0x100) {
      return false;
    }
    output_.push_back(char(c));
    return true;
  }

  const std::string& GetOutput() const { return output_; }

 private:
  const u8* input_;
  std::string output_;
};

/** Wide char16_t format string source and sink. */
class WideStringFormatData {
 public:
  /**
   * Constructs the source over a format string.
   *
   * @param input Host pointer to the NUL terminated big endian UTF-16 format string.
   */
  explicit WideStringFormatData(const u16* input) : input_(input) {}

  /**
   * Returns the next character and advances, or returns 0 at the end without advancing.
   *
   * @return The character.
   */
  u16 get() {
    u16 result = memory::load<be_u16>(input_);
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return memory::load<be_u16>(input_ + offset); }

  /**
   * Advances past up to count characters, stopping at the end.
   *
   * @param count Number of characters to skip.
   */
  void skip(i32 count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  /**
   * Appends a character to the output.
   *
   * @param c The character.
   * @return True always.
   */
  bool put(u16 c) {
    output_.push_back(char16_t(c));
    return true;
  }

  const std::u16string& GetWstr() const { return output_; }

 private:
  const u16* input_;
  std::u16string output_;
};

/** Counts wide output without storing it. */
class WideCountFormatData {
 public:
  /**
   * Constructs the source over a format string.
   *
   * @param input Host pointer to the NUL terminated big endian UTF-16 format string.
   */
  explicit WideCountFormatData(const u16* input) : input_(input), count_(0) {}

  /**
   * Returns the next character and advances, or returns 0 at the end without advancing.
   *
   * @return The character.
   */
  u16 get() {
    u16 result = memory::load<be_u16>(input_);
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return memory::load<be_u16>(input_ + offset); }

  /**
   * Advances past up to count characters, stopping at the end.
   *
   * @param count Number of characters to skip.
   */
  void skip(i32 count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  /**
   * Counts a character without storing it.
   *
   * @param c The character, unused.
   * @return True always.
   */
  bool put([[maybe_unused]] u16 c) {
    ++count_;
    return true;
  }

  i32 GetCount() const { return count_; }

 private:
  const u16* input_;
  i32 count_;
};

/**
 * Runs the printf format string state machine.
 *
 * @tparam Data The format string source and sink.
 * @tparam Args The argument list source.
 * @param base The host base of guest memory.
 * @param data The format string source and sink.
 * @param args The argument list.
 * @param wide True when the format string is wide.
 * @return The number of characters written, or -1 on failure.
 */
template <detail::FormatDataSource Data, detail::ArgListSource Args>
i32 FormatCore(u8* base, Data& data, Args& args, const bool wide) {
  i32 count = 0;

  char work8[512];
  char16_t work16[4];
  std::string float_text;

  struct {
    const void* buffer;
    i32 length;
    bool is_wide;
    bool swap_wide;
  } text;

  struct {
    char buffer[2];
    i32 length;
  } prefix;

  auto state = detail::kFSUnknown;
  u32 flags = 0;
  i32 width = 0;
  i32 precision = -1;
  [[maybe_unused]] detail::ArgumentSize size = detail::kASDefault;
  i32 radix = 0;
  const char* digits = nullptr;

  text.buffer = nullptr;
  text.is_wide = false;
  text.swap_wide = true;
  text.length = 0;
  prefix.buffer[0] = '\0';
  prefix.length = 0;

  for (u16 c = data.get();; c = data.get()) {
    if (state == detail::kFSUnknown) {
      if (!c) {
        return count;
      } else if (c != '%') {
      output:
        if (!data.put(c)) {
          return -1;
        }
        ++count;
        continue;
      }

      state = detail::kFSStart;
      c = data.get();
    }

    if (!c) {
      return -1;
    }

  restart:
    switch (state) {
      case detail::kFSInvalid:
      case detail::kFSUnknown:
      case detail::kFSEnd:
      default: {
        REX_ASSERT_ALWAYS();
      }

      case detail::kFSStart: {
        if (c == '%') {
          state = detail::kFSUnknown;
          goto output;
        }

        state = detail::kFSFlags;

        flags = 0;
        width = 0;
        precision = -1;
        size = detail::kASDefault;
        radix = 0;
        digits = nullptr;

        text.buffer = nullptr;
        text.is_wide = false;
        text.swap_wide = true;
        text.length = 0;
        prefix.buffer[0] = '\0';
        prefix.length = 0;

        [[fallthrough]];
      }

      case detail::kFSFlags: {
        if (c == '-') {
          flags |= detail::kFFLeftJustify;
          continue;
        } else if (c == '+') {
          flags |= detail::kFFAddPositive;
          continue;
        } else if (c == '0') {
          flags |= detail::kFFAddLeadingZeros;
          continue;
        } else if (c == ' ') {
          flags |= detail::kFFAddPositiveAsSpace;
          continue;
        } else if (c == '#') {
          flags |= detail::kFFAddPrefix;
          continue;
        }
        state = detail::kFSWidth;
        [[fallthrough]];
      }

      case detail::kFSWidth: {
        if (c == '*') {
          width = static_cast<i32>(args.get32());
          if (width < 0) {
            flags |= detail::kFFLeftJustify;
            width = -width;
          }
          state = detail::kFSPrecisionStart;
          continue;
        } else if (c >= '0' && c <= '9') {
          width *= 10;
          width += c - '0';
          continue;
        }
        state = detail::kFSPrecisionStart;
        [[fallthrough]];
      }

      case detail::kFSPrecisionStart: {
        if (c == '.') {
          state = detail::kFSPrecision;
          precision = 0;
          continue;
        }
        state = detail::kFSSize;
        goto restart;
      }

      case detail::kFSPrecision: {
        if (c == '*') {
          precision = static_cast<i32>(args.get32());
          if (precision < 0) {
            precision = -1;
          }
          state = detail::kFSSize;
          continue;
        } else if (c >= '0' && c <= '9') {
          precision *= 10;
          precision += c - '0';
          continue;
        }
        state = detail::kFSSize;
        [[fallthrough]];
      }

      case detail::kFSSize: {
        if (c == 'l') {
          if (data.peek(0) == 'l') {
            data.skip(1);
            flags |= detail::kFFIsLongLong;
          } else {
            flags |= detail::kFFIsLong;
          }
          state = detail::kFSType;
          continue;
        } else if (c == 'L') {
          state = detail::kFSType;
          continue;
        } else if (c == 'h') {
          flags |= detail::kFFIsShort;
          state = detail::kFSType;
          continue;
        } else if (c == 'w') {
          flags |= detail::kFFIsWide;
          state = detail::kFSType;
          continue;
        } else if (c == 'I') {
          if (data.peek(0) == '6' && data.peek(1) == '4') {
            data.skip(2);
            flags |= detail::kFFIsLongLong;
            state = detail::kFSType;
            continue;
          } else if (data.peek(0) == '3' && data.peek(1) == '2') {
            data.skip(2);
            state = detail::kFSType;
            continue;
          } else {
            state = detail::kFSType;
            continue;
          }
        }
        [[fallthrough]];
      }

      case detail::kFSType: {
        switch (c) {
          case 'C': {
            flags |= detail::kFFInvertWide;
            [[fallthrough]];
          }

          case 'c': {
            bool is_wide;
            if (flags & (detail::kFFIsLong | detail::kFFIsWide)) {
              is_wide = true;
            } else if (flags & detail::kFFIsShort) {
              is_wide = false;
            } else {
              is_wide = ((flags & detail::kFFInvertWide) != 0) ^ wide;
            }

            auto value = args.get32();

            if (!is_wide) {
              work8[0] = static_cast<u8>(value);
              text.buffer = &work8[0];
              text.length = 1;
              text.is_wide = false;
            } else {
              work16[0] = static_cast<u16>(value);
              text.buffer = &work16[0];
              text.length = 1;
              text.is_wide = true;
              text.swap_wide = false;
            }

            break;
          }

          case 'd':
          case 'i': {
            flags |= detail::kFFIsSigned;
            digits = "0123456789";
            radix = 10;

          integer:
            REX_ASSERT(digits != nullptr);
            REX_ASSERT(radix != 0);

            i64 value;

            if (flags & detail::kFFIsLongLong) {
              value = static_cast<i64>(args.get64());
            } else if (flags & detail::kFFIsShort) {
              value = static_cast<i16>(args.get32());
            } else {
              value = static_cast<i32>(args.get32());
            }

            if (precision >= 0) {
              precision = std::min(precision, static_cast<i32>(std::size(work8)));
            } else {
              precision = 1;
            }

            if ((flags & detail::kFFIsSigned) && value < 0) {
              value = -value;
              flags |= detail::kFFAddNegative;
            }

            if (!(flags & detail::kFFIsLongLong)) {
              value &= UINT32_MAX;
            }

            if (value == 0) {
              prefix.length = 0;
            }

            char* end = work8 + std::size(work8);
            char* start = end;

            while (precision-- > 0 || value != 0) {
              auto digit = static_cast<i32>(value % radix);
              value /= radix;
              REX_ASSERT(digit < static_cast<i32>(strlen(digits)));
              *--start = digits[digit];
            }

            if ((flags & detail::kFFForceLeadingZero) && (start == end || *start != '0')) {
              *--start = '0';
            }

            text.buffer = start;
            text.length = static_cast<i32>(end - start);
            text.is_wide = false;
            break;
          }

          case 'o': {
            digits = "01234567";
            radix = 8;
            if (flags & detail::kFFAddPrefix) {
              flags |= detail::kFFForceLeadingZero;
            }
            goto integer;
          }

          case 'u': {
            digits = "0123456789";
            radix = 10;
            goto integer;
          }

          case 'x':
          case 'X': {
            digits = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
            radix = 16;

            if (flags & detail::kFFAddPrefix) {
              prefix.buffer[0] = '0';
              prefix.buffer[1] = c == 'x' ? 'x' : 'X';
              prefix.length = 2;
            }

            goto integer;
          }

          case 'e':
          case 'E':
          case 'f':
          case 'g':
          case 'G':
          case 'a':
          case 'A': {
            flags |= detail::kFFIsSigned;

            i64 dummy = args.get64();
            f64 value = std::bit_cast<f64>(dummy);

            if (value < 0) {
              value = -value;
              flags |= detail::kFFAddNegative;
            }

            float_text = detail::FormatDouble(
                value, std::min(precision, static_cast<i32>(std::size(work8))), c, flags);

            text.buffer = float_text.c_str();
            text.length = static_cast<i32>(float_text.size());
            text.is_wide = false;
            break;
          }

          case 'n': {
            auto pointer = static_cast<u32>(args.get32());
            if (flags & detail::kFFIsShort) {
              *rex::runtime::GuestPtr<be_u16*>(base, pointer) = static_cast<u16>(count);
            } else {
              *rex::runtime::GuestPtr<be_u32*>(base, pointer) = static_cast<u32>(count);
            }
            continue;
          }

          case 'p': {
            digits = "0123456789ABCDEF";
            radix = 16;
            precision = 8;
            flags &= ~(detail::kFFIsLongLong | detail::kFFIsShort);
            flags |= detail::kFFIsLong;
            goto integer;
          }

          case 'S': {
            flags |= detail::kFFInvertWide;
            [[fallthrough]];
          }

          case 's': {
            u32 pointer = args.get32();
            i32 cap = precision < 0 ? INT32_MAX : precision;

            if (pointer == 0) {
              auto nullstr = "(null)";
              text.buffer = nullstr;
              text.length = std::min(static_cast<i32>(strlen(nullstr)), cap);
              text.is_wide = false;
            } else {
              void* str = rex::runtime::GuestPtr<void*>(base, pointer);
              bool is_wide;
              if (flags & (detail::kFFIsLong | detail::kFFIsWide)) {
                is_wide = true;
              } else if (flags & detail::kFFIsShort) {
                is_wide = false;
              } else {
                is_wide = ((flags & detail::kFFInvertWide) != 0) ^ wide;
              }
              i32 length;

              if (!is_wide) {
                length = 0;
                for (auto s = static_cast<const u8*>(str); cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              } else {
                length = 0;
                for (auto s = static_cast<const u16*>(str); cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              }

              text.buffer = str;
              text.length = length;
              text.is_wide = is_wide;
            }
            break;
          }

          case 'Z': {
            const u32 pointer = args.get32();
            const u32 buffer_pointer =
                pointer
                    ? static_cast<u32>(*rex::runtime::GuestPtr<const be_u32*>(base, pointer + 4))
                    : 0;
            if (!buffer_pointer) {
              static constexpr char kNullString[] = "(null)";
              text.buffer = kNullString;
              text.length = static_cast<i32>(std::size(kNullString) - 1);
              text.is_wide = false;
            } else {
              const i32 bytes = static_cast<i16>(
                  static_cast<u16>(*rex::runtime::GuestPtr<const be_u16*>(base, pointer)));
              text.is_wide = (flags & detail::kFFIsWide) != 0;
              text.length = std::max(text.is_wide ? bytes / 2 : bytes, 0);
              text.buffer = rex::runtime::GuestPtr<void*>(base, buffer_pointer);
            }
            break;
          }

          default: {
            return -1;
          }
        }
      }
    }

    if (flags & detail::kFFIsSigned) {
      if (flags & detail::kFFAddNegative) {
        prefix.buffer[0] = '-';
        prefix.length = 1;
      } else if (flags & detail::kFFAddPositive) {
        prefix.buffer[0] = '+';
        prefix.length = 1;
      } else if (flags & detail::kFFAddPositiveAsSpace) {
        prefix.buffer[0] = ' ';
        prefix.length = 1;
      }
    }

    i32 padding = width - text.length - prefix.length;

    if (!(flags & (detail::kFFLeftJustify | detail::kFFAddLeadingZeros)) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    if (prefix.length > 0) {
      i32 remaining = prefix.length;
      count += prefix.length;
      auto b = &prefix.buffer[0];
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    }

    if ((flags & detail::kFFAddLeadingZeros) && !(flags & detail::kFFLeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put('0')) {
          return -1;
        }
      }
    }

    i32 remaining = text.length;
    if (!text.is_wide) {
      auto b = static_cast<const u8*>(text.buffer);
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    } else {
      auto b = static_cast<const u16*>(text.buffer);
      if (text.swap_wide) {
        while (remaining-- > 0) {
          if (!data.put(memory::load<be_u16>(b++))) {
            return -1;
          }
        }
      } else {
        while (remaining-- > 0) {
          if (!data.put(*b++)) {
            return -1;
          }
        }
      }
    }
    count += text.length;

    if ((flags & detail::kFFLeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    state = detail::kFSUnknown;
  }

  return count;
}

}  // namespace rex::runtime
