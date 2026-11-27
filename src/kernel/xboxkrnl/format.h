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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/assert.h>
#include <rex/kernel/memory_view.h>
#include <rex/memory/endian.h>
#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/types.h>

namespace rex::kernel::xboxkrnl::detail {

/** States of the printf format specifier parser. */
enum FormatState : u8 {
  kFSInvalid = 0,
  kFSLiteral, /**< Copying literal text outside a specifier. */
  kFSStart,   /**< After the percent sign. */
  kFSFlags,
  kFSWidth,
  kFSPrecisionStart,
  kFSPrecision,
  kFSSize,
  kFSType,
  kFSEnd,
};

/** Flags collected while parsing a printf format specifier. */
enum FormatFlags : u16 {
  kFFLeftJustify = 1 << 0,
  kFFAddLeadingZeros = 1 << 1,    /**< Pad the field with zeros. */
  kFFAddPositive = 1 << 2,        /**< Print a plus sign on positive values. */
  kFFAddPositiveAsSpace = 1 << 3, /**< Print a space on positive values. */
  kFFAddNegative = 1 << 4,        /**< The value is negative, so print a minus sign. */
  kFFAddPrefix = 1 << 5,          /**< Print the alternate form prefix. */
  kFFIsShort = 1 << 6,
  kFFIsLong = 1 << 7,
  kFFIsLongLong = 1 << 8,
  kFFIsWide = 1 << 9,
  kFFIsSigned = 1 << 10,
  kFFForceLeadingZero = 1 << 11, /**< Octal conversion with the alternate form. */
  kFFInvertWide = 1 << 12,       /**< Flips the width of the format string character type. */
};

/** Size prefix of a printf conversion. */
enum ArgumentSize : u8 {
  kASDefault = 0,
  kASShort,
  kASLong,
  kASLongLong,
};

template <typename T>
concept FormatDataSource = requires(T& t, i32 n, u16 c) {
  { t.GetNext() } -> std::same_as<u16>;
  { t.peek(n) } -> std::same_as<u16>;
  { t.skip(n) } -> std::same_as<void>;
  { t.TryPut(c) } -> std::same_as<bool>;
};

template <typename T>
concept ArgListSource = requires(T& t) {
  { t.get32() } -> std::same_as<u32>;
  { t.get64() } -> std::same_as<u64>;
};

template <typename T>
concept FormatMemory = requires(T& t, u32 pointer, bool wide, i32 cap, i32 count, bool is_short,
                                const void*& buffer, i32& length) {
  { t.GetString(pointer, wide, cap, buffer, length) } -> std::same_as<bool>;
  { t.GetCountedString(pointer, wide, buffer, length) } -> std::same_as<bool>;
  { t.StoreCount(pointer, count, is_short) } -> std::same_as<bool>;
};

/** Reads and writes the guest memory a conversion names without checking the addresses. */
class DirectMemory {
 public:
  /**
   * Binds the engine to guest memory.
   *
   * @param base The host base of guest memory.
   */
  explicit DirectMemory(u8* base) : base_(base) {}

  /**
   * Finds the text of a %s or %S argument.
   *
   * @param pointer The guest address of the string, not 0.
   * @param wide True when the string is UTF-16.
   * @param cap The most characters to take.
   * @param buffer Receives the host address of the text.
   * @param length Receives the length in characters.
   * @return True.
   */
  bool GetString(u32 pointer, bool wide, i32 cap, const void*& buffer, i32& length) const {
    const void* str = rex::memory::GuestPtr<void*>(base_, pointer);
    length = 0;
    if (!wide) {
      for (auto s = static_cast<const u8*>(str); cap > 0 && *s; ++s, cap--) {
        length++;
      }
    } else {
      for (auto s = static_cast<const u16*>(str); cap > 0 && *s; ++s, cap--) {
        length++;
      }
    }
    buffer = str;
    return true;
  }

  /**
   * Finds the text of a %Z argument.
   *
   * @param pointer The guest address of the counted string, not 0.
   * @param wide True when the string is UTF-16.
   * @param buffer Receives the host address of the text, null when the string has no buffer.
   * @param length Receives the length in characters.
   * @return True.
   */
  bool GetCountedString(u32 pointer, bool wide, const void*& buffer, i32& length) const {
    const u32 buffer_pointer =
        static_cast<u32>(*rex::memory::GuestPtr<const be_u32*>(base_, pointer + 4));
    if (!buffer_pointer) {
      buffer = nullptr;
      length = 0;
      return true;
    }
    const i32 bytes =
        static_cast<i16>(static_cast<u16>(*rex::memory::GuestPtr<const be_u16*>(base_, pointer)));
    length = std::max(wide ? bytes / 2 : bytes, 0);
    buffer = rex::memory::GuestPtr<void*>(base_, buffer_pointer);
    return true;
  }

  /**
   * Writes the count of a %n conversion.
   *
   * @param pointer The guest address the conversion names.
   * @param count The characters written so far.
   * @param is_short True for %hn.
   * @return True.
   */
  bool StoreCount(u32 pointer, i32 count, bool is_short) const {
    if (is_short) {
      *rex::memory::GuestPtr<be_u16*>(base_, pointer) = static_cast<u16>(count);
    } else {
      *rex::memory::GuestPtr<be_u32*>(base_, pointer) = static_cast<u32>(count);
    }
    return true;
  }

 private:
  u8* base_;
};

/**
 * Reads the guest memory a conversion names only where the title could read it, and writes none.
 * A %n conversion stores nothing.
 */
class CheckedMemory {
 public:
  /**
   * Binds the engine to guest memory.
   *
   * @param view The guest memory of the calling thread.
   */
  explicit CheckedMemory(const rex::kernel::MemoryView& view) : view_(view) {}

  /**
   * Finds the text of a %s or %S argument, cut short at unreadable memory.
   *
   * @param pointer The guest address of the string, not 0.
   * @param wide True when the string is UTF-16.
   * @param cap The most characters to take.
   * @param buffer Receives the host address of the text.
   * @param length Receives the length in characters.
   * @return False when the first character is not readable.
   */
  bool GetString(u32 pointer, bool wide, i32 cap, const void*& buffer, i32& length) {
    if (!wide) {
      const auto text = view_.TryGetCString(pointer, static_cast<size_t>(cap));
      if (!text) {
        return Fail();
      }
      buffer = text->data();
      length = static_cast<i32>(text->size());
      return true;
    }
    wide_.clear();
    wide_.reserve(1);
    for (i32 index = 0; index < cap; ++index) {
      const auto c = LoadAt<u16>(pointer, static_cast<u64>(index) * 2);
      if (!c) {
        if (index == 0) {
          return Fail();
        }
        break;
      }
      if (*c == 0) {
        break;
      }
      wide_.emplace_back(*c);
    }
    buffer = wide_.data();
    length = static_cast<i32>(wide_.size());
    return true;
  }

  /**
   * Copies the text of a %Z argument.
   *
   * @param pointer The guest address of the counted string, not 0.
   * @param wide True when the string is UTF-16.
   * @param buffer Receives the host address of the copy, null when the string has no buffer.
   * @param length Receives the length in characters.
   * @return False when the header or a character is not readable.
   */
  bool GetCountedString(u32 pointer, bool wide, const void*& buffer, i32& length) {
    const auto buffer_pointer = LoadAt<u32>(pointer, 4);
    const auto bytes = view_.TryLoad<u16>(pointer);
    if (!buffer_pointer || !bytes) {
      return Fail();
    }
    if (*buffer_pointer == 0) {
      buffer = nullptr;
      length = 0;
      return true;
    }
    const i32 signed_bytes = static_cast<i16>(*bytes);
    const i32 count = std::max(wide ? signed_bytes / 2 : signed_bytes, 0);
    narrow_.clear();
    wide_.clear();
    wide_.reserve(1);
    for (i32 index = 0; index < count; ++index) {
      if (wide) {
        const auto c = LoadAt<u16>(*buffer_pointer, static_cast<u64>(index) * 2);
        if (!c) {
          return Fail();
        }
        wide_.emplace_back(*c);
      } else {
        const auto c = LoadAt<u8>(*buffer_pointer, static_cast<u64>(index));
        if (!c) {
          return Fail();
        }
        narrow_.push_back(static_cast<char>(*c));
      }
    }
    buffer = wide ? static_cast<const void*>(wide_.data()) : narrow_.c_str();
    length = count;
    return true;
  }

  /**
   * Consumes a %n conversion and stores nothing.
   *
   * @return True.
   */
  bool StoreCount([[maybe_unused]] u32 pointer, [[maybe_unused]] i32 count,
                  [[maybe_unused]] bool is_short) const {
    return true;
  }

  /** True when a string argument was not readable. */
  bool IsUnreadable() const { return unreadable_; }

 private:
  template <typename T>
  std::optional<T> LoadAt(u32 address, u64 offset) const {
    const u64 at = static_cast<u64>(address) + offset;
    if (at > 0xFFFFFFFFull) {
      return std::nullopt;
    }
    return view_.TryLoad<T>(static_cast<u32>(at));
  }

  bool Fail() {
    unreadable_ = true;
    return false;
  }

  const rex::kernel::MemoryView& view_;
  std::string narrow_;
  std::vector<be_u16> wide_;
  bool unreadable_ = false;
};

/**
 * Formats a floating point value for the e, E, f, g, G, a and A conversions.
 *
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

/**
 * Reads variadic arguments from PPC registers and the stack. Arguments 0 to 7 come from r3 to r10,
 * the rest from the 8 byte argument image slots at r1 + 0x50, where a 32 bit value sits in the low
 * word.
 */
class StackArgList {
 public:
  /**
   * Reads variadic arguments starting after the fixed parameters.
   *
   * @param base The host base of guest memory.
   * @param start_index Index of the first variadic argument, which counts the fixed parameters.
   */
  StackArgList(PPCContext& ctx, u8* base, i32 start_index)
      : ctx_(ctx), base_(base), index_(start_index) {}

  u32 get32() {
    return static_cast<u32>(rex::ppc::ArgTranslator::GetIntegerArgumentValue<sizeof(u32)>(
        ctx_, base_, static_cast<size_t>(index_++)));
  }

  u64 get64() {
    return rex::ppc::ArgTranslator::GetIntegerArgumentValue<sizeof(u64)>(
        ctx_, base_, static_cast<size_t>(index_++));
  }

 private:
  PPCContext& ctx_;
  u8* base_;
  i32 index_;
};

/** Reads arguments from a guest va_list, a pointer to an array of 8 byte aligned values. */
class ArrayArgList {
 public:
  ArrayArgList(u8* base, u32 arg_ptr) : base_(base), arg_ptr_(arg_ptr), index_(0) {}

  u32 get32() { return static_cast<u32>(get64()); }

  u64 get64() {
    u32 addr = arg_ptr_ + (8 * index_);
    u64 value = static_cast<u64>(*rex::memory::GuestPtr<be_u64*>(base_, addr));
    ++index_;
    return value;
  }

 private:
  u8* base_;
  u32 arg_ptr_;
  i32 index_;
};

/**
 * Reads variadic arguments as StackArgList does, but reads a stack slot only when the title could
 * read it.
 */
class CheckedStackArgList {
 public:
  /**
   * Reads variadic arguments starting at an argument index.
   *
   * @param ctx The register file at the entry of the function that received the arguments.
   * @param view The guest memory of the calling thread.
   * @param start_index Index of the first argument to read, which counts the fixed parameters.
   */
  CheckedStackArgList(const PPCContext& ctx, const rex::kernel::MemoryView& view, u32 start_index)
      : ctx_(ctx), view_(view), index_(start_index) {}

  u32 get32() { return static_cast<u32>(get64()); }

  /** Returns the next argument, 0 when its stack slot is not readable. */
  u64 get64() {
    const size_t index = index_++;
    if (index < rex::ppc::ArgTranslator::kIntegerRegisters) {
      return rex::ppc::ArgTranslator::GetIntegerRegister(const_cast<PPCContext&>(ctx_), index).u64;
    }
    const auto value =
        view_.TryLoad<u64>(rex::ppc::ArgTranslator::GetStackSlotAddress(ctx_, index));
    if (!value) {
      unreadable_ = true;
      return 0;
    }
    return *value;
  }

  /** True when a stack slot was not readable. */
  bool IsUnreadable() const { return unreadable_; }

 private:
  const PPCContext& ctx_;
  const rex::kernel::MemoryView& view_;
  u32 index_;
  bool unreadable_ = false;
};

/** Reads arguments from a guest va_list as ArrayArgList does, but only where the title could. */
class CheckedArrayArgList {
 public:
  /**
   * Reads the values of a va_list.
   *
   * @param view The guest memory of the calling thread.
   * @param arg_ptr The guest address of the first value.
   */
  CheckedArrayArgList(const rex::kernel::MemoryView& view, u32 arg_ptr)
      : view_(view), arg_ptr_(arg_ptr) {}

  u32 get32() { return static_cast<u32>(get64()); }

  /** Returns the next value, 0 when it is not readable. */
  u64 get64() {
    const u64 address = static_cast<u64>(arg_ptr_) + static_cast<u64>(index_++) * 8;
    const auto value =
        address <= 0xFFFFFFF8ull ? view_.TryLoad<u64>(static_cast<u32>(address)) : std::nullopt;
    if (!value) {
      unreadable_ = true;
      return 0;
    }
    return *value;
  }

  /** True when a value was not readable. */
  bool IsUnreadable() const { return unreadable_; }

 private:
  const rex::kernel::MemoryView& view_;
  u32 arg_ptr_;
  u32 index_ = 0;
  bool unreadable_ = false;
};

/** Narrow format string source and sink. */
class StringFormatData {
 public:
  explicit StringFormatData(const u8* input) : input_(input) {}

  /** Returns the next character and advances, or 0 at the end. */
  u16 GetNext() {
    u16 result = *input_;
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return input_[offset]; }

  void skip(i32 count) {
    while (count-- > 0) {
      if (!GetNext()) {
        break;
      }
    }
  }

  /**
   * Appends a character to the output.
   *
   * @return False when c does not fit in a narrow character.
   */
  bool TryPut(u16 c) {
    if (c >= 0x100) {
      return false;
    }
    output_.push_back(static_cast<char>(c));
    return true;
  }

  const std::string& GetOutput() const { return output_; }

 private:
  const u8* input_;
  std::string output_;
};

/** Narrow format string source and a sink that keeps at most a given number of bytes. */
class BoundedStringFormatData {
 public:
  /**
   * Formats a copied format string.
   *
   * @param input The format string, without its terminator.
   * @param max_length The most bytes the output keeps.
   */
  BoundedStringFormatData(std::string_view input, size_t max_length)
      : input_(input), max_length_(max_length) {}

  /** Returns the next character and advances, or 0 at the end. */
  u16 GetNext() {
    const u16 result = peek(0);
    if (result) {
      position_++;
    }
    return result;
  }

  u16 peek(i32 offset) {
    const size_t at = position_ + static_cast<size_t>(offset);
    return at < input_.size() ? static_cast<u8>(input_[at]) : 0;
  }

  void skip(i32 count) {
    while (count-- > 0) {
      if (!GetNext()) {
        break;
      }
    }
  }

  /**
   * Appends a character to the output.
   *
   * @return False when c does not fit in a narrow character or the output is full.
   */
  bool TryPut(u16 c) {
    if (c >= 0x100) {
      return false;
    }
    if (output_.size() >= max_length_) {
      truncated_ = true;
      return false;
    }
    output_.push_back(static_cast<char>(c));
    return true;
  }

  std::string& GetOutput() { return output_; }

  /** True when the output reached max_length and a character was refused. */
  bool IsTruncated() const { return truncated_; }

 private:
  std::string_view input_;
  size_t max_length_;
  size_t position_ = 0;
  std::string output_;
  bool truncated_ = false;
};

/** Wide format string source and sink. */
class WideStringFormatData {
 public:
  explicit WideStringFormatData(const u16* input) : input_(input) {}

  /** Returns the next character and advances, or 0 at the end. */
  u16 GetNext() {
    u16 result = memory::load<be_u16>(input_);
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return memory::load<be_u16>(input_ + offset); }

  void skip(i32 count) {
    while (count-- > 0) {
      if (!GetNext()) {
        break;
      }
    }
  }

  bool TryPut(u16 c) {
    output_.push_back(static_cast<char16_t>(c));
    return true;
  }

  const std::u16string& GetOutput() const { return output_; }

 private:
  const u16* input_;
  std::u16string output_;
};

/** Counts wide output without storing it. */
class WideCountFormatData {
 public:
  explicit WideCountFormatData(const u16* input) : input_(input), count_(0) {}

  /** Returns the next character and advances, or 0 at the end. */
  u16 GetNext() {
    u16 result = memory::load<be_u16>(input_);
    if (result) {
      input_++;
    }
    return result;
  }

  u16 peek(i32 offset) { return memory::load<be_u16>(input_ + offset); }

  void skip(i32 count) {
    while (count-- > 0) {
      if (!GetNext()) {
        break;
      }
    }
  }

  bool TryPut([[maybe_unused]] u16 c) {
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
 * @tparam Memory The access to the guest memory that string and count conversions name.
 * @tparam Data The format string source and sink.
 * @tparam Args The argument list source.
 * @param memory The access to the guest memory that string and count conversions name.
 * @param data The format string source and sink.
 * @param wide True when the format string is wide.
 * @return The number of characters written, or -1 on failure.
 */
template <FormatMemory Memory, FormatDataSource Data, ArgListSource Args>
i32 FormatCore(Memory& memory, Data& data, Args& args, const bool wide) {
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

  auto state = kFSLiteral;
  u32 flags = 0;
  i32 width = 0;
  i32 precision = -1;
  [[maybe_unused]] ArgumentSize size = kASDefault;
  i32 radix = 0;
  const char* digits = nullptr;

  text.buffer = nullptr;
  text.is_wide = false;
  text.swap_wide = true;
  text.length = 0;
  prefix.buffer[0] = '\0';
  prefix.length = 0;

  for (u16 c = data.GetNext();; c = data.GetNext()) {
    if (state == kFSLiteral) {
      if (!c) {
        return count;
      } else if (c != '%') {
      output:
        if (!data.TryPut(c)) {
          return -1;
        }
        ++count;
        continue;
      }

      state = kFSStart;
      c = data.GetNext();
    }

    if (!c) {
      return -1;
    }

  restart:
    switch (state) {
      case kFSInvalid:
      case kFSLiteral:
      case kFSEnd:
      default: {
        REX_ASSERT_ALWAYS();
      }

      case kFSStart: {
        if (c == '%') {
          state = kFSLiteral;
          goto output;
        }

        state = kFSFlags;

        flags = 0;
        width = 0;
        precision = -1;
        size = kASDefault;
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

      case kFSFlags: {
        if (c == '-') {
          flags |= kFFLeftJustify;
          continue;
        } else if (c == '+') {
          flags |= kFFAddPositive;
          continue;
        } else if (c == '0') {
          flags |= kFFAddLeadingZeros;
          continue;
        } else if (c == ' ') {
          flags |= kFFAddPositiveAsSpace;
          continue;
        } else if (c == '#') {
          flags |= kFFAddPrefix;
          continue;
        }
        state = kFSWidth;
        [[fallthrough]];
      }

      case kFSWidth: {
        if (c == '*') {
          width = static_cast<i32>(args.get32());
          if (width < 0) {
            flags |= kFFLeftJustify;
            width = -width;
          }
          state = kFSPrecisionStart;
          continue;
        } else if (c >= '0' && c <= '9') {
          width *= 10;
          width += c - '0';
          continue;
        }
        state = kFSPrecisionStart;
        [[fallthrough]];
      }

      case kFSPrecisionStart: {
        if (c == '.') {
          state = kFSPrecision;
          precision = 0;
          continue;
        }
        state = kFSSize;
        goto restart;
      }

      case kFSPrecision: {
        if (c == '*') {
          precision = static_cast<i32>(args.get32());
          if (precision < 0) {
            precision = -1;
          }
          state = kFSSize;
          continue;
        } else if (c >= '0' && c <= '9') {
          precision *= 10;
          precision += c - '0';
          continue;
        }
        state = kFSSize;
        [[fallthrough]];
      }

      case kFSSize: {
        if (c == 'l') {
          if (data.peek(0) == 'l') {
            data.skip(1);
          } else {
            flags |= kFFIsLong;
          }
          state = kFSType;
          continue;
        } else if (c == 'L') {
          state = kFSType;
          continue;
        } else if (c == 'h') {
          flags |= kFFIsShort;
          state = kFSType;
          continue;
        } else if (c == 'w') {
          flags |= kFFIsWide;
          state = kFSType;
          continue;
        } else if (c == 'I') {
          if (data.peek(0) == '6' && data.peek(1) == '4') {
            data.skip(2);
            flags |= kFFIsLongLong;
            state = kFSType;
            continue;
          } else if (data.peek(0) == '3' && data.peek(1) == '2') {
            data.skip(2);
            state = kFSType;
            continue;
          } else {
            state = kFSType;
            continue;
          }
        }
        [[fallthrough]];
      }

      case kFSType: {
        switch (c) {
          case 'C': {
            flags |= kFFInvertWide;
            [[fallthrough]];
          }

          case 'c': {
            bool is_wide;
            if (flags & (kFFIsLong | kFFIsWide)) {
              is_wide = true;
            } else if (flags & kFFIsShort) {
              is_wide = false;
            } else {
              is_wide = ((flags & kFFInvertWide) != 0) ^ wide;
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
            flags |= kFFIsSigned;
            digits = "0123456789";
            radix = 10;

          integer:
            REX_ASSERT(digits != nullptr);
            REX_ASSERT(radix != 0);

            i64 value;

            if (flags & kFFIsLongLong) {
              value = static_cast<i64>(args.get64());
            } else if (flags & kFFIsShort) {
              value = static_cast<i16>(args.get32());
            } else {
              value = static_cast<i32>(args.get32());
            }

            if (precision >= 0) {
              precision = std::min(precision, static_cast<i32>(std::size(work8)));
            } else {
              precision = 1;
            }

            if ((flags & kFFIsSigned) && value < 0) {
              value = -value;
              flags |= kFFAddNegative;
            }

            if (!(flags & kFFIsLongLong)) {
              value &= UINT32_MAX;
            }

            if (value == 0) {
              prefix.length = 0;
            }

            char* end = work8 + std::size(work8);
            char* start = end;

            u64 magnitude = static_cast<u64>(value);
            while (precision-- > 0 || magnitude != 0) {
              auto digit = static_cast<i32>(magnitude % radix);
              magnitude /= radix;
              REX_ASSERT(digit < static_cast<i32>(strlen(digits)));
              *--start = digits[digit];
            }

            if ((flags & kFFForceLeadingZero) && (start == end || *start != '0')) {
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
            if (flags & kFFAddPrefix) {
              flags |= kFFForceLeadingZero;
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

            if (flags & kFFAddPrefix) {
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
            flags |= kFFIsSigned;

            i64 bits = args.get64();
            f64 value = std::bit_cast<f64>(bits);

            if (value < 0) {
              value = -value;
              flags |= kFFAddNegative;
            }

            float_text = FormatDouble(
                value, std::min(precision, static_cast<i32>(std::size(work8))), c, flags);

            text.buffer = float_text.c_str();
            text.length = static_cast<i32>(float_text.size());
            text.is_wide = false;
            break;
          }

          case 'n': {
            auto pointer = static_cast<u32>(args.get32());
            if (!memory.StoreCount(pointer, count, (flags & kFFIsShort) != 0)) {
              return -1;
            }
            state = kFSLiteral;
            continue;
          }

          case 'p': {
            digits = "0123456789ABCDEF";
            radix = 16;
            precision = 8;
            flags &= ~(kFFIsLongLong | kFFIsShort);
            flags |= kFFIsLong;
            goto integer;
          }

          case 'S': {
            flags |= kFFInvertWide;
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
              bool is_wide;
              if (flags & (kFFIsLong | kFFIsWide)) {
                is_wide = true;
              } else if (flags & kFFIsShort) {
                is_wide = false;
              } else {
                is_wide = ((flags & kFFInvertWide) != 0) ^ wide;
              }
              if (!memory.GetString(pointer, is_wide, cap, text.buffer, text.length)) {
                return -1;
              }
              text.is_wide = is_wide;
            }
            break;
          }

          case 'Z': {
            const u32 pointer = args.get32();
            const bool is_wide = (flags & kFFIsWide) != 0;
            text.buffer = nullptr;
            if (pointer && !memory.GetCountedString(pointer, is_wide, text.buffer, text.length)) {
              return -1;
            }
            if (!text.buffer) {
              static constexpr char kNullString[] = "(null)";
              text.buffer = kNullString;
              text.length = static_cast<i32>(std::size(kNullString) - 1);
              text.is_wide = false;
            } else {
              text.is_wide = is_wide;
            }
            break;
          }

          default: {
            state = kFSLiteral;
            goto output;
          }
        }
      }
    }

    if (flags & kFFIsSigned) {
      if (flags & kFFAddNegative) {
        prefix.buffer[0] = '-';
        prefix.length = 1;
      } else if (flags & kFFAddPositive) {
        prefix.buffer[0] = '+';
        prefix.length = 1;
      } else if (flags & kFFAddPositiveAsSpace) {
        prefix.buffer[0] = ' ';
        prefix.length = 1;
      }
    }

    i32 padding = width - text.length - prefix.length;

    if (!(flags & (kFFLeftJustify | kFFAddLeadingZeros)) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.TryPut(' ')) {
          return -1;
        }
      }
    }

    if (prefix.length > 0) {
      i32 remaining = prefix.length;
      count += prefix.length;
      auto b = &prefix.buffer[0];
      while (remaining-- > 0) {
        if (!data.TryPut(*b++)) {
          return -1;
        }
      }
    }

    if ((flags & kFFAddLeadingZeros) && !(flags & kFFLeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.TryPut('0')) {
          return -1;
        }
      }
    }

    i32 remaining = text.length;
    if (!text.is_wide) {
      auto b = static_cast<const u8*>(text.buffer);
      while (remaining-- > 0) {
        if (!data.TryPut(*b++)) {
          return -1;
        }
      }
    } else {
      auto b = static_cast<const u16*>(text.buffer);
      if (text.swap_wide) {
        while (remaining-- > 0) {
          if (!data.TryPut(memory::load<be_u16>(b++))) {
            return -1;
          }
        }
      } else {
        while (remaining-- > 0) {
          if (!data.TryPut(*b++)) {
            return -1;
          }
        }
      }
    }
    count += text.length;

    if ((flags & kFFLeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.TryPut(' ')) {
          return -1;
        }
      }
    }

    state = kFSLiteral;
  }

  return count;
}

}  // namespace rex::kernel::xboxkrnl::detail
