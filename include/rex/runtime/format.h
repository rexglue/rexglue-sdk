/**
 * @file        runtime/format.h
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
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/types.h>

namespace rex::runtime::format {

/** States of the printf format specifier parser. */
enum FormatState {
  FS_Invalid = 0,    /**< Invalid state. */
  FS_Unknown,        /**< Copying literal text outside a specifier. */
  FS_Start,          /**< After the percent sign. */
  FS_Flags,          /**< Reading flag characters. */
  FS_Width,          /**< Reading the field width. */
  FS_PrecisionStart, /**< Checking for a precision. */
  FS_Precision,      /**< Reading the precision. */
  FS_Size,           /**< Reading the size prefix. */
  FS_Type,           /**< Reading the conversion character. */
  FS_End,            /**< Specifier complete. */
};

/** Flags collected while parsing a printf format specifier. */
enum FormatFlags {
  FF_LeftJustify = 1 << 0,        /**< Left justify the field. */
  FF_AddLeadingZeros = 1 << 1,    /**< Pad the field with zeros. */
  FF_AddPositive = 1 << 2,        /**< Print a plus sign on positive values. */
  FF_AddPositiveAsSpace = 1 << 3, /**< Print a space on positive values. */
  FF_AddNegative = 1 << 4,        /**< The value is negative, so print a minus sign. */
  FF_AddPrefix = 1 << 5,          /**< Print the alternate form prefix. */
  FF_IsShort = 1 << 6,            /**< Short size prefix. */
  FF_IsLong = 1 << 7,             /**< Long size prefix. */
  FF_IsLongLong = 1 << 8,         /**< Long long size prefix. */
  FF_IsWide = 1 << 9,             /**< Wide size prefix. */
  FF_IsSigned = 1 << 10,          /**< Signed integer conversion. */
  FF_ForceLeadingZero = 1 << 11,  /**< Octal conversion with the alternate form. */
  FF_InvertWide = 1 << 12,        /**< Flips the width of the format string character type. */
};

/** Size prefix of a printf conversion. */
enum ArgumentSize {
  AS_Default = 0, /**< No size prefix. */
  AS_Short,       /**< Short size prefix. */
  AS_Long,        /**< Long size prefix. */
  AS_LongLong,    /**< Long long size prefix. */
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
inline std::string format_double(f64 value, i32 precision, u16 c, u32 flags) {
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

  if (flags & FF_AddPrefix) {
    temp << std::showpoint;
  }

  temp << value;
  return temp.str();
}

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

  const std::string& str() const { return output_; }

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

  const std::u16string& wstr() const { return output_; }

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

  i32 count() const { return count_; }

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
template <FormatDataSource Data, ArgListSource Args>
i32 format_core(u8* base, Data& data, Args& args, const bool wide) {
  i32 count = 0;

  char work8[512];
  char16_t work16[4];

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

  auto state = FS_Unknown;
  u32 flags = 0;
  i32 width = 0;
  i32 precision = -1;
  [[maybe_unused]] ArgumentSize size = AS_Default;
  i32 radix = 0;
  const char* digits = nullptr;

  text.buffer = nullptr;
  text.is_wide = false;
  text.swap_wide = true;
  text.length = 0;
  prefix.buffer[0] = '\0';
  prefix.length = 0;

  for (u16 c = data.get();; c = data.get()) {
    if (state == FS_Unknown) {
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

      state = FS_Start;
      c = data.get();
    }

    if (!c) {
      return -1;
    }

  restart:
    switch (state) {
      case FS_Invalid:
      case FS_Unknown:
      case FS_End:
      default: {
        REX_ASSERT_ALWAYS();
      }

      case FS_Start: {
        if (c == '%') {
          state = FS_Unknown;
          goto output;
        }

        state = FS_Flags;

        flags = 0;
        width = 0;
        precision = -1;
        size = AS_Default;
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

      case FS_Flags: {
        if (c == '-') {
          flags |= FF_LeftJustify;
          continue;
        } else if (c == '+') {
          flags |= FF_AddPositive;
          continue;
        } else if (c == '0') {
          flags |= FF_AddLeadingZeros;
          continue;
        } else if (c == ' ') {
          flags |= FF_AddPositiveAsSpace;
          continue;
        } else if (c == '#') {
          flags |= FF_AddPrefix;
          continue;
        }
        state = FS_Width;
        [[fallthrough]];
      }

      case FS_Width: {
        if (c == '*') {
          width = static_cast<i32>(args.get32());
          if (width < 0) {
            flags |= FF_LeftJustify;
            width = -width;
          }
          state = FS_PrecisionStart;
          continue;
        } else if (c >= '0' && c <= '9') {
          width *= 10;
          width += c - '0';
          continue;
        }
        state = FS_PrecisionStart;
        [[fallthrough]];
      }

      case FS_PrecisionStart: {
        if (c == '.') {
          state = FS_Precision;
          precision = 0;
          continue;
        }
        state = FS_Size;
        goto restart;
      }

      case FS_Precision: {
        if (c == '*') {
          precision = static_cast<i32>(args.get32());
          if (precision < 0) {
            precision = -1;
          }
          state = FS_Size;
          continue;
        } else if (c >= '0' && c <= '9') {
          precision *= 10;
          precision += c - '0';
          continue;
        }
        state = FS_Size;
        [[fallthrough]];
      }

      case FS_Size: {
        if (c == 'l') {
          if (data.peek(0) == 'l') {
            data.skip(1);
            flags |= FF_IsLongLong;
          } else {
            flags |= FF_IsLong;
          }
          state = FS_Type;
          continue;
        } else if (c == 'L') {
          state = FS_Type;
          continue;
        } else if (c == 'h') {
          flags |= FF_IsShort;
          state = FS_Type;
          continue;
        } else if (c == 'w') {
          flags |= FF_IsWide;
          state = FS_Type;
          continue;
        } else if (c == 'I') {
          if (data.peek(0) == '6' && data.peek(1) == '4') {
            data.skip(2);
            flags |= FF_IsLongLong;
            state = FS_Type;
            continue;
          } else if (data.peek(0) == '3' && data.peek(1) == '2') {
            data.skip(2);
            state = FS_Type;
            continue;
          } else {
            state = FS_Type;
            continue;
          }
        }
        [[fallthrough]];
      }

      case FS_Type: {
        switch (c) {
          case 'C': {
            flags |= FF_InvertWide;
            [[fallthrough]];
          }

          case 'c': {
            bool is_wide;
            if (flags & (FF_IsLong | FF_IsWide)) {
              is_wide = true;
            } else if (flags & FF_IsShort) {
              is_wide = false;
            } else {
              is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
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
            flags |= FF_IsSigned;
            digits = "0123456789";
            radix = 10;

          integer:
            REX_ASSERT(digits != nullptr);
            REX_ASSERT(radix != 0);

            i64 value;

            if (flags & FF_IsLongLong) {
              value = static_cast<i64>(args.get64());
            } else if (flags & FF_IsLong) {
              value = static_cast<i32>(args.get32());
            } else if (flags & FF_IsShort) {
              value = static_cast<i16>(args.get32());
            } else {
              value = static_cast<i32>(args.get32());
            }

            if (precision >= 0) {
              precision = std::min(precision, static_cast<i32>(std::size(work8)));
            } else {
              precision = 1;
            }

            if ((flags & FF_IsSigned) && value < 0) {
              value = -value;
              flags |= FF_AddNegative;
            }

            if (!(flags & FF_IsLongLong)) {
              value &= UINT32_MAX;
            }

            if (value == 0) {
              prefix.length = 0;
            }

            char* end = &work8[std::size(work8) - 1];
            char* start = end;
            start[0] = '\0';

            while (precision-- > 0 || value != 0) {
              auto digit = static_cast<i32>(value % radix);
              value /= radix;
              REX_ASSERT(digit < static_cast<i32>(strlen(digits)));
              *--start = digits[digit];
            }

            if ((flags & FF_ForceLeadingZero) && (start == end || *start != '0')) {
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
            if (flags & FF_AddPrefix) {
              flags |= FF_ForceLeadingZero;
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

            if (flags & FF_AddPrefix) {
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
            flags |= FF_IsSigned;

            i64 dummy = args.get64();
            f64 value = std::bit_cast<f64>(dummy);

            if (value < 0) {
              value = -value;
              flags |= FF_AddNegative;
            }

            auto s = format_double(value, precision, c, flags);
            auto length = static_cast<i32>(s.size());
            REX_ASSERT(length < static_cast<i32>(std::size(work8)));

            auto fstart = &work8[0];
            auto fend = &fstart[length];

            std::memcpy(fstart, s.c_str(), length);
            fend[0] = '\0';

            text.buffer = fstart;
            text.length = static_cast<i32>(fend - fstart);
            text.is_wide = false;
            break;
          }

          case 'n': {
            auto pointer = static_cast<u32>(args.get32());
            if (flags & FF_IsShort) {
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
            flags &= ~(FF_IsLongLong | FF_IsShort);
            flags |= FF_IsLong;
            goto integer;
          }

          case 'S': {
            flags |= FF_InvertWide;
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
              if (flags & (FF_IsLong | FF_IsWide)) {
                is_wide = true;
              } else if (flags & FF_IsShort) {
                is_wide = false;
              } else {
                is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
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
            REX_ASSERT_ALWAYS();
            break;
          }

          default: {
            REX_ASSERT_ALWAYS();
          }
        }
      }
    }

    if (flags & FF_IsSigned) {
      if (flags & FF_AddNegative) {
        prefix.buffer[0] = '-';
        prefix.length = 1;
      } else if (flags & FF_AddPositive) {
        prefix.buffer[0] = '+';
        prefix.length = 1;
      } else if (flags & FF_AddPositiveAsSpace) {
        prefix.buffer[0] = ' ';
        prefix.length = 1;
      }
    }

    i32 padding = width - text.length - prefix.length;

    if (!(flags & (FF_LeftJustify | FF_AddLeadingZeros)) && padding > 0) {
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

    if ((flags & FF_AddLeadingZeros) && !(flags & (FF_LeftJustify)) && padding > 0) {
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

    if ((flags & FF_LeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    state = FS_Unknown;
  }

  return count;
}

}  // namespace rex::runtime::format
