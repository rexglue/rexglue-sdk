/**
 * @file        kernel/format.h
 * @brief       PPC printf format engine for kernel string exports
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on Xenia's format_core
 */

#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#include <rex/assert.h>
#include <rex/byte_order.h>
#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/memory.h>

namespace rex::kernel::format {

enum FormatState {
  FS_Invalid = 0,
  FS_Unknown,
  FS_Start,
  FS_Flags,
  FS_Width,
  FS_PrecisionStart,
  FS_Precision,
  FS_Size,
  FS_Type,
  FS_End,
};

enum FormatFlags {
  FF_LeftJustify = 1 << 0,
  FF_AddLeadingZeros = 1 << 1,
  FF_AddPositive = 1 << 2,
  FF_AddPositiveAsSpace = 1 << 3,
  FF_AddNegative = 1 << 4,
  FF_AddPrefix = 1 << 5,
  FF_IsShort = 1 << 6,
  FF_IsLong = 1 << 7,
  FF_IsLongLong = 1 << 8,
  FF_IsWide = 1 << 9,
  FF_IsSigned = 1 << 10,
  FF_ForceLeadingZero = 1 << 11,
  FF_InvertWide = 1 << 12,
};

enum ArgumentSize {
  AS_Default = 0,
  AS_Short,
  AS_Long,
  AS_LongLong,
};

//=============================================================================
// Concepts
//=============================================================================
template<typename T>
concept FormatDataSource = requires(T& t, int32_t n, uint16_t c) {
    { t.get() } -> std::same_as<uint16_t>;
    { t.peek(n) } -> std::same_as<uint16_t>;
    { t.skip(n) } -> std::same_as<void>;
    { t.put(c) } -> std::same_as<bool>;
};

template<typename T>
concept ArgListSource = requires(T& t) {
    { t.get32() } -> std::same_as<uint32_t>;
    { t.get64() } -> std::same_as<uint64_t>;
};

//=============================================================================
// format_double helper
//=============================================================================
inline std::string format_double(double value, int32_t precision, uint16_t c,
                                 uint32_t flags) {
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

//=============================================================================
// StackArgList — reads variadic args from PPC registers/stack
//=============================================================================
// For sprintf, _snprintf, DbgPrint, etc. where varargs follow fixed params.
// Xbox 360 PPC has 64-bit GPRs. Variadic args in r3-r10, then stack at
// r1+0x54 in 8-byte slots.
class StackArgList {
 public:
  StackArgList(PPCContext& ctx, uint8_t* base, int32_t start_index)
      : ctx_(ctx), base_(base), index_(start_index) {}

  uint32_t get32() { return static_cast<uint32_t>(get64()); }

  uint64_t get64() {
    uint64_t value;
    if (index_ <= 7) {
      switch (index_) {
        case 0: value = ctx_.r3.u64; break;
        case 1: value = ctx_.r4.u64; break;
        case 2: value = ctx_.r5.u64; break;
        case 3: value = ctx_.r6.u64; break;
        case 4: value = ctx_.r7.u64; break;
        case 5: value = ctx_.r8.u64; break;
        case 6: value = ctx_.r9.u64; break;
        case 7: value = ctx_.r10.u64; break;
        default: value = 0; break;
      }
    } else {
      // Stack arguments: 8-byte big-endian values at r1 + 0x54 + ((index - 8) * 8)
      uint32_t stack_addr = ctx_.r1.u32 + 0x54 + ((index_ - 8) * 8);
      value = __builtin_bswap64(
          *reinterpret_cast<uint64_t*>(base_ + stack_addr + PPC_PHYS_HOST_OFFSET(stack_addr)));
    }
    ++index_;
    return value;
  }

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  int32_t index_;
};

//=============================================================================
// ArrayArgList — reads from guest va_list (8-byte-aligned array in memory)
//=============================================================================
// For vsprintf, _vsnprintf, etc. where a va_list pointer is passed.
// On Xbox 360, va_list is a pointer to an array of 8-byte-aligned values.
class ArrayArgList {
 public:
  ArrayArgList(uint8_t* base, uint32_t arg_ptr)
      : base_(base), arg_ptr_(arg_ptr), index_(0) {}

  uint32_t get32() { return static_cast<uint32_t>(get64()); }

  uint64_t get64() {
    uint32_t addr = arg_ptr_ + (8 * index_);
    uint64_t value = __builtin_bswap64(
        *reinterpret_cast<uint64_t*>(base_ + addr + PPC_PHYS_HOST_OFFSET(addr)));
    ++index_;
    return value;
  }

 private:
  uint8_t* base_;
  uint32_t arg_ptr_;
  int32_t index_;
};

//=============================================================================
// StringFormatData — narrow char format string I/O
//=============================================================================
class StringFormatData {
 public:
  explicit StringFormatData(const uint8_t* input) : input_(input) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return result;
  }

  uint16_t peek(int32_t offset) { return input_[offset]; }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    if (c >= 0x100) {
      return false;
    }
    output_.push_back(char(c));
    return true;
  }

  const std::string& str() const { return output_; }

 private:
  const uint8_t* input_;
  std::string output_;
};

//=============================================================================
// WideStringFormatData — wide (char16_t) format string I/O
//=============================================================================
class WideStringFormatData {
 public:
  explicit WideStringFormatData(const uint16_t* input) : input_(input) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return rex::byte_swap(result);
  }

  uint16_t peek(int32_t offset) { return rex::byte_swap(input_[offset]); }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    output_.push_back(char16_t(c));
    return true;
  }

  const std::u16string& wstr() const { return output_; }

 private:
  const uint16_t* input_;
  std::u16string output_;
};

//=============================================================================
// WideCountFormatData — counts wide output without storing
//=============================================================================
class WideCountFormatData {
 public:
  explicit WideCountFormatData(const uint16_t* input) : input_(input), count_(0) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return rex::byte_swap(result);
  }

  uint16_t peek(int32_t offset) { return rex::byte_swap(input_[offset]); }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    ++count_;
    return true;
  }

  int32_t count() const { return count_; }

 private:
  const uint16_t* input_;
  int32_t count_;
};

//=============================================================================
// format_core — printf format string state machine
//=============================================================================
// Reference: https://msdn.microsoft.com/en-us/library/56e442dc.aspx
template<FormatDataSource Data, ArgListSource Args>
int32_t format_core(uint8_t* base, Data& data, Args& args, const bool wide) {
  int32_t count = 0;

  char work8[512];
  char16_t work16[4];

  struct {
    const void* buffer;
    int32_t length;
    bool is_wide;
    bool swap_wide;
  } text;

  struct {
    char buffer[2];
    int32_t length;
  } prefix;

  auto state = FS_Unknown;
  uint32_t flags = 0;
  int32_t width = 0;
  int32_t precision = -1;
  [[maybe_unused]] ArgumentSize size = AS_Default;
  int32_t radix = 0;
  const char* digits = nullptr;

  text.buffer = nullptr;
  text.is_wide = false;
  text.swap_wide = true;
  text.length = 0;
  prefix.buffer[0] = '\0';
  prefix.length = 0;

  for (uint16_t c = data.get();; c = data.get()) {
    if (state == FS_Unknown) {
      if (!c) {  // the end
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
      // fall through
    }

    // in any state, if c is \0, it's bad
    if (!c) {
      return -1;
    }

  restart:
    switch (state) {
      case FS_Invalid:
      case FS_Unknown:
      case FS_End:
      default: {
        assert_always();
      }

      case FS_Start: {
        if (c == '%') {
          state = FS_Unknown;
          goto output;
        }

        state = FS_Flags;

        // reset to defaults
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

        // fall through, don't need to goto restart
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
        // fall through
      }

      case FS_Width: {
        if (c == '*') {
          width = (int32_t)args.get32();
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
        // fall through
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
          precision = (int32_t)args.get32();
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
        // fall through
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
        // fall through
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
              work8[0] = (uint8_t)value;
              text.buffer = &work8[0];
              text.length = 1;
              text.is_wide = false;
            } else {
              work16[0] = (uint16_t)value;
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
            assert_not_null(digits);
            assert_not_zero(radix);

            int64_t value;

            if (flags & FF_IsLongLong) {
              value = (int64_t)args.get64();
            } else if (flags & FF_IsLong) {
              value = (int32_t)args.get32();
            } else if (flags & FF_IsShort) {
              value = (int16_t)args.get32();
            } else {
              value = (int32_t)args.get32();
            }

            if (precision >= 0) {
              precision = std::min(precision, (int32_t)std::size(work8));
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
              auto digit = (int32_t)(value % radix);
              value /= radix;
              assert_true(digit < (int32_t)strlen(digits));
              *--start = digits[digit];
            }

            if ((flags & FF_ForceLeadingZero) &&
                (start == end || *start != '0')) {
              *--start = '0';
            }

            text.buffer = start;
            text.length = (int32_t)(end - start);
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

            int64_t dummy = args.get64();
            double value;
            std::memcpy(&value, &dummy, sizeof(double));

            if (value < 0) {
              value = -value;
              flags |= FF_AddNegative;
            }

            auto s = format_double(value, precision, c, flags);
            auto length = (int32_t)s.size();
            assert_true(length < (int32_t)std::size(work8));

            auto fstart = &work8[0];
            auto fend = &fstart[length];

            std::memcpy(fstart, s.c_str(), length);
            fend[0] = '\0';

            text.buffer = fstart;
            text.length = (int32_t)(fend - fstart);
            text.is_wide = false;
            break;
          }

          // %n: write character count to guest memory
          case 'n': {
            auto pointer = (uint32_t)args.get32();
            if (flags & FF_IsShort) {
              PPC_STORE_U16(pointer, (uint16_t)count);
            } else {
              PPC_STORE_U32(pointer, (uint32_t)count);
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
            uint32_t pointer = args.get32();
            int32_t cap = precision < 0 ? INT32_MAX : precision;

            if (pointer == 0) {
              auto nullstr = "(null)";
              text.buffer = nullstr;
              text.length = std::min((int32_t)strlen(nullstr), cap);
              text.is_wide = false;
            } else {
              void* str = reinterpret_cast<void*>(
                  base + pointer + PPC_PHYS_HOST_OFFSET(pointer));
              bool is_wide;
              if (flags & (FF_IsLong | FF_IsWide)) {
                is_wide = true;
              } else if (flags & FF_IsShort) {
                is_wide = false;
              } else {
                is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
              }
              int32_t length;

              if (!is_wide) {
                length = 0;
                for (auto s = (const uint8_t*)str; cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              } else {
                length = 0;
                for (auto s = (const uint16_t*)str; cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              }

              text.buffer = str;
              text.length = length;
              text.is_wide = is_wide;
            }
            break;
          }

          // ANSI_STRING / UNICODE_STRING (not implemented)
          case 'Z': {
            assert_always();
            break;
          }

          default: {
            assert_always();
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

    int32_t padding = width - text.length - prefix.length;

    if (!(flags & (FF_LeftJustify | FF_AddLeadingZeros)) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    if (prefix.length > 0) {
      int32_t remaining = prefix.length;
      count += prefix.length;
      auto b = &prefix.buffer[0];
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    }

    if ((flags & FF_AddLeadingZeros) && !(flags & (FF_LeftJustify)) &&
        padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put('0')) {
          return -1;
        }
      }
    }

    int32_t remaining = text.length;
    if (!text.is_wide) {
      auto b = (const uint8_t*)text.buffer;
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    } else {
      auto b = (const uint16_t*)text.buffer;
      if (text.swap_wide) {
        while (remaining-- > 0) {
          if (!data.put(rex::byte_swap(*b++))) {
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

    // right padding
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

}  // namespace rex::kernel::format
