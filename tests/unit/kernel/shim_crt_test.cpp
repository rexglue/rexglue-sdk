/**
 * @file        tests/unit/kernel/shim_crt_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>

#include <rex/ppc/context.h>
#include <rex/ppc/call.h>
#include <rex/ppc/recompiled_code.h>

using namespace rex::ppc;

extern "C" REX_FUNC(rexshim_memcpy);
extern "C" REX_FUNC(rexshim_memmove);
extern "C" REX_FUNC(rexshim_memset);
extern "C" REX_FUNC(rexshim_memchr);
extern "C" REX_FUNC(rexshim_memcpy_s);
extern "C" REX_FUNC(rexshim_memmove_s);
extern "C" REX_FUNC(rexshim_strncmp);
extern "C" REX_FUNC(rexshim_strncpy);
extern "C" REX_FUNC(rexshim_strchr);
extern "C" REX_FUNC(rexshim_strstr);
extern "C" REX_FUNC(rexshim_strrchr);
extern "C" REX_FUNC(rexshim_strcpy_s);
extern "C" REX_FUNC(rexshim_wcslen);
extern "C" REX_FUNC(rexshim_wcscmp);
extern "C" REX_FUNC(rexshim_wcsncmp);
extern "C" REX_FUNC(rexshim_wcschr);
extern "C" REX_FUNC(rexshim_wcsrchr);
extern "C" REX_FUNC(rexshim_wcscpy);
extern "C" REX_FUNC(rexshim_wcsncpy);
extern "C" REX_FUNC(rexshim_wcsncpy_s);
extern "C" REX_FUNC(rexshim_wcscoll);
extern "C" REX_FUNC(rexshim__stricmp);
extern "C" REX_FUNC(rexshim_wcsstr);

namespace {

using HookFn = void (*)(PPCContext&, u8*);

alignas(64) u8 g_guest[0x10000];

constexpr u32 kSource = 0x1000;
constexpr u32 kDestination = 0x2000;
constexpr u32 kGuard = 0xEE;

void ResetGuest() {
  std::memset(g_guest, kGuard, sizeof(g_guest));
}

u64 Call(HookFn hook, std::initializer_list<u64> arguments) {
  PPCContext ctx{};
  u32 index = 0;
  for (const u64 value : arguments) {
    ArgTranslator::SetIntegerArgumentValue(ctx, g_guest, index++, value);
  }
  hook(ctx, g_guest);
  return ctx.r3.u64;
}

u32 CallAddress(HookFn hook, std::initializer_list<u64> arguments) {
  return static_cast<u32>(Call(hook, arguments));
}

int CallInt(HookFn hook, std::initializer_list<u64> arguments) {
  return static_cast<int>(static_cast<i32>(Call(hook, arguments)));
}

void PutBytes(u32 address, std::string_view bytes) {
  std::memcpy(g_guest + address, bytes.data(), bytes.size());
}

void PutString(u32 address, std::string_view text) {
  PutBytes(address, text);
  g_guest[address + text.size()] = 0;
}

void PutWide(u32 address, std::u16string_view text) {
  for (size_t i = 0; i <= text.size(); ++i) {
    const char16_t unit = i < text.size() ? text[i] : u'\0';
    g_guest[address + i * 2] = static_cast<u8>(unit >> 8);
    g_guest[address + i * 2 + 1] = static_cast<u8>(unit & 0xFF);
  }
}

std::u16string GetWide(u32 address) {
  std::u16string result;
  for (;; address += 2) {
    const auto unit = static_cast<char16_t>((g_guest[address] << 8) | g_guest[address + 1]);
    if (unit == 0) {
      return result;
    }
    result.push_back(unit);
  }
}

std::string GetBytes(u32 address, size_t size) {
  return std::string(reinterpret_cast<const char*>(g_guest + address), size);
}

bool IsUntouched(u32 address, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (g_guest[address + i] != kGuard) {
      return false;
    }
  }
  return true;
}

}  // namespace

REX_TEST_CASE("CRT memcpy copies the requested bytes and returns the destination",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, "0123456789");
  CHECK(CallAddress(rexshim_memcpy, {kDestination, kSource, 10}) == kDestination);
  CHECK(GetBytes(kDestination, 10) == "0123456789");
  CHECK(IsUntouched(kDestination + 10, 16));
}

REX_TEST_CASE("CRT memcpy of zero bytes writes nothing", "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, "abcd");
  CHECK(CallAddress(rexshim_memcpy, {kDestination, kSource, 0}) == kDestination);
  CHECK(IsUntouched(kDestination, 16));
}

REX_TEST_CASE("CRT memcpy handles unaligned addresses and sizes", "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource + 1, "abcdefghijklmnopq");
  CHECK(CallAddress(rexshim_memcpy, {kDestination + 3, kSource + 1, 17}) == kDestination + 3);
  CHECK(GetBytes(kDestination + 3, 17) == "abcdefghijklmnopq");
  CHECK(IsUntouched(kDestination, 3));
  CHECK(IsUntouched(kDestination + 20, 8));
}

REX_TEST_CASE("CRT memmove handles overlap in both directions", "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, "0123456789");
  CHECK(CallAddress(rexshim_memmove, {kSource + 3, kSource, 7}) == kSource + 3);
  CHECK(GetBytes(kSource, 10) == "0120123456");
  PutBytes(kSource, "0123456789");
  CHECK(CallAddress(rexshim_memmove, {kSource, kSource + 3, 7}) == kSource);
  CHECK(GetBytes(kSource, 10) == "3456789789");
}

REX_TEST_CASE("CRT memmove of zero bytes writes nothing", "[kernel][shim][crt]") {
  ResetGuest();
  CHECK(CallAddress(rexshim_memmove, {kDestination, kDestination + 1, 0}) == kDestination);
  CHECK(IsUntouched(kDestination, 16));
}

REX_TEST_CASE("CRT memset fills with the low byte of the value", "[kernel][shim][crt]") {
  ResetGuest();
  CHECK(CallAddress(rexshim_memset, {kDestination, 0x1AB, 5}) == kDestination);
  CHECK(GetBytes(kDestination, 5) == std::string(5, '\xAB'));
  CHECK(IsUntouched(kDestination + 5, 8));
  CHECK(CallAddress(rexshim_memset, {kDestination + 8, 0x7F, 0}) == kDestination + 8);
  CHECK(IsUntouched(kDestination + 8, 8));
}

REX_TEST_CASE("CRT memchr finds the first match and reports a miss as null",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, std::string_view("ab\xC3"
                                     "cd\xC3",
                                     6));
  CHECK(CallAddress(rexshim_memchr, {kSource, 'c', 6}) == kSource + 3);
  CHECK(CallAddress(rexshim_memchr, {kSource, 0xC3, 6}) == kSource + 2);
  CHECK(CallAddress(rexshim_memchr, {kSource, 'z', 6}) == 0);
  CHECK(CallAddress(rexshim_memchr, {kSource, 'd', 4}) == 0);
  CHECK(CallAddress(rexshim_memchr, {kSource, 'a', 0}) == 0);
}

REX_TEST_CASE("CRT memcpy_s and memmove_s copy within the destination size",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, "abcdefgh");
  CHECK(CallInt(rexshim_memcpy_s, {kDestination, 8, kSource, 8}) == 0);
  CHECK(GetBytes(kDestination, 8) == "abcdefgh");
  CHECK(CallInt(rexshim_memmove_s, {kSource + 2, 6, kSource, 6}) == 0);
  CHECK(GetBytes(kSource, 8) == "ababcdef");
}

REX_TEST_CASE("CRT memcpy_s and memmove_s refuse a count past the destination size",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kSource, "abcdefgh");
  PutBytes(kDestination, "wxyzwxyz");
  CHECK(CallInt(rexshim_memcpy_s, {kDestination, 4, kSource, 8}) == 34);
  CHECK(GetBytes(kDestination, 8) == std::string("\0\0\0\0wxyz", 8));
  PutBytes(kDestination, "wxyzwxyz");
  CHECK(CallInt(rexshim_memmove_s, {kDestination, 4, kSource, 8}) == 34);
  CHECK(GetBytes(kDestination, 8) == "wxyzwxyz");
}

REX_TEST_CASE(
    "CRT memcpy_s zeroes the destination for a null source and refuses a null destination",
    "[kernel][shim][crt]") {
  ResetGuest();
  PutBytes(kDestination, "wxyzwxyz");
  CHECK(CallInt(rexshim_memcpy_s, {kDestination, 8, 0, 8}) == 22);
  CHECK(GetBytes(kDestination, 8) == std::string(8, '\0'));
  CHECK(CallInt(rexshim_memcpy_s, {0, 8, kSource, 8}) == 22);
  CHECK(CallInt(rexshim_memmove_s, {kDestination, 8, 0, 8}) == 22);
  CHECK(CallInt(rexshim_memmove_s, {0, 8, kSource, 8}) == 22);
}

REX_TEST_CASE("CRT memcpy_s and memmove_s accept a count of zero before any check",
              "[kernel][shim][crt]") {
  ResetGuest();
  CHECK(CallInt(rexshim_memcpy_s, {0, 0, 0, 0}) == 0);
  CHECK(CallInt(rexshim_memmove_s, {0, 0, 0, 0}) == 0);
  CHECK(CallInt(rexshim_memcpy_s, {kDestination, 0, 0, 0}) == 0);
  CHECK(IsUntouched(kDestination, 16));
}

REX_TEST_CASE("CRT strncmp compares up to a length", "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "abcdef");
  PutString(kDestination, "abcxyz");
  CHECK(CallInt(rexshim_strncmp, {kSource, kDestination, 3}) == 0);
  CHECK(CallInt(rexshim_strncmp, {kSource, kDestination, 4}) < 0);
  CHECK(CallInt(rexshim_strncmp, {kDestination, kSource, 4}) > 0);
  CHECK(CallInt(rexshim_strncmp, {kSource, kDestination, 0}) == 0);
}

REX_TEST_CASE("CRT strncpy pads with zeros and does not terminate a full copy",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "abc");
  CHECK(CallAddress(rexshim_strncpy, {kDestination, kSource, 6}) == kDestination);
  CHECK(GetBytes(kDestination, 6) == std::string("abc\0\0\0", 6));
  CHECK(IsUntouched(kDestination + 6, 8));
  ResetGuest();
  PutString(kSource, "abcdef");
  CHECK(CallAddress(rexshim_strncpy, {kDestination, kSource, 3}) == kDestination);
  CHECK(GetBytes(kDestination, 3) == "abc");
  CHECK(IsUntouched(kDestination + 3, 8));
}

REX_TEST_CASE("CRT strchr strrchr and strstr return guest addresses", "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "hello world");
  CHECK(CallAddress(rexshim_strchr, {kSource, 'o'}) == kSource + 4);
  CHECK(CallAddress(rexshim_strchr, {kSource, 'z'}) == 0);
  CHECK(CallAddress(rexshim_strchr, {kSource, 0}) == kSource + 11);
  CHECK(CallAddress(rexshim_strrchr, {kSource, 'o'}) == kSource + 7);
  CHECK(CallAddress(rexshim_strrchr, {kSource, 'z'}) == 0);
  PutString(kDestination, "wor");
  CHECK(CallAddress(rexshim_strstr, {kSource, kDestination}) == kSource + 6);
  PutString(kDestination, "xyz");
  CHECK(CallAddress(rexshim_strstr, {kSource, kDestination}) == 0);
}

REX_TEST_CASE("CRT strcpy_s copies and refuses a short destination", "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "abcd");
  CHECK(CallInt(rexshim_strcpy_s, {kDestination, 5, kSource}) == 0);
  CHECK(GetBytes(kDestination, 5) == std::string("abcd\0", 5));
  CHECK(CallInt(rexshim_strcpy_s, {kDestination, 0, kSource}) == 22);
  CHECK(CallInt(rexshim_strcpy_s, {0, 5, kSource}) == 22);
}

REX_TEST_CASE("CRT strcpy_s copies an overflow and then clears the first byte",
              "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "abcd");
  CHECK(CallInt(rexshim_strcpy_s, {kDestination, 4, kSource}) == 34);
  CHECK(GetBytes(kDestination, 4) == std::string("\0bcd", 4));
  CHECK(g_guest[kDestination + 4] == kGuard);
}

REX_TEST_CASE("CRT strcpy_s clears the first byte for a null source", "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kDestination, "xyz");
  CHECK(CallInt(rexshim_strcpy_s, {kDestination, 4, 0}) == 22);
  CHECK(GetBytes(kDestination, 4) == std::string("\0yz\0", 4));
}

REX_TEST_CASE("CRT _stricmp folds ASCII case and reports a null argument", "[kernel][shim][crt]") {
  ResetGuest();
  PutString(kSource, "AbC_");
  PutString(kDestination, "aBcD");
  CHECK(CallInt(rexshim__stricmp, {kSource, kDestination}) == '_' - 'd');
  PutString(kDestination, "aBc_");
  CHECK(CallInt(rexshim__stricmp, {kSource, kDestination}) == 0);
  g_guest[kSource] = 0xC0;
  g_guest[kSource + 1] = 0;
  g_guest[kDestination] = 0xE0;
  g_guest[kDestination + 1] = 0;
  CHECK(CallInt(rexshim__stricmp, {kSource, kDestination}) == 0xC0 - 0xE0);
  CHECK(CallInt(rexshim__stricmp, {0, kDestination}) == 0x7FFFFFFF);
  CHECK(CallInt(rexshim__stricmp, {kSource, 0}) == 0x7FFFFFFF);
}

REX_TEST_CASE("CRT wcslen counts big endian units", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abc\x100");
  CHECK(CallInt(rexshim_wcslen, {kSource}) == 4);
  PutWide(kSource, u"");
  CHECK(CallInt(rexshim_wcslen, {kSource}) == 0);
}

REX_TEST_CASE("CRT wcscmp orders by code unit value", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abc");
  PutWide(kDestination, u"abc");
  CHECK(CallInt(rexshim_wcscmp, {kSource, kDestination}) == 0);
  PutWide(kDestination, u"abd");
  CHECK(CallInt(rexshim_wcscmp, {kSource, kDestination}) < 0);
  CHECK(CallInt(rexshim_wcscmp, {kDestination, kSource}) > 0);
  PutWide(kSource, u"\x100");
  PutWide(kDestination, u"\xFF");
  CHECK(CallInt(rexshim_wcscmp, {kSource, kDestination}) > 0);
  CHECK(CallInt(rexshim_wcscmp, {kDestination, kSource}) < 0);
  PutWide(kSource, u"\x201");
  PutWide(kDestination, u"\x102");
  CHECK(CallInt(rexshim_wcscmp, {kSource, kDestination}) > 0);
  CHECK(CallInt(rexshim_wcscmp, {kDestination, kSource}) < 0);
}

REX_TEST_CASE("CRT wcscmp returns only minus one, zero or one", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"a");
  PutWide(kDestination, u"");
  CHECK(CallInt(rexshim_wcscmp, {kSource, kDestination}) == -1);
  CHECK(CallInt(rexshim_wcscmp, {kDestination, kSource}) == 1);
}

REX_TEST_CASE("CRT wcsncmp compares with a count of two to the thirty first or more",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abc");
  PutWide(kDestination, u"abd");
  CHECK(CallInt(rexshim_wcsncmp, {kSource, kDestination, 0x80000000u}) < 0);
  CHECK(CallInt(rexshim_wcsncmp, {kDestination, kSource, 0xFFFFFFFFu}) > 0);
  CHECK(CallInt(rexshim_wcsncmp, {kSource, kDestination, 0}) == 0);
}

REX_TEST_CASE("CRT wcsncmp orders by code unit value up to a length", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcd");
  PutWide(kDestination, u"abxy");
  CHECK(CallInt(rexshim_wcsncmp, {kSource, kDestination, 2}) == 0);
  CHECK(CallInt(rexshim_wcsncmp, {kSource, kDestination, 3}) < 0);
  PutWide(kSource, u"\x201");
  PutWide(kDestination, u"\x102");
  CHECK(CallInt(rexshim_wcsncmp, {kSource, kDestination, 1}) > 0);
}

REX_TEST_CASE("CRT wcschr and wcsrchr compare against big endian units",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcabc\x141");
  CHECK(CallAddress(rexshim_wcschr, {kSource, 'b'}) == kSource + 2);
  CHECK(CallAddress(rexshim_wcschr, {kSource, 'z'}) == 0);
  CHECK(CallAddress(rexshim_wcschr, {kSource, 0x141}) == kSource + 12);
  CHECK(CallAddress(rexshim_wcschr, {kSource, 0}) == kSource + 14);
  CHECK(CallAddress(rexshim_wcsrchr, {kSource, 'b'}) == kSource + 8);
  CHECK(CallAddress(rexshim_wcsrchr, {kSource, 'z'}) == 0);
}

REX_TEST_CASE("CRT wcscpy copies the second argument into the first", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"copy me");
  PutWide(kDestination, u"old text that is longer");
  CHECK(CallAddress(rexshim_wcscpy, {kDestination, kSource}) == kDestination);
  CHECK(GetWide(kDestination) == u"copy me");
  CHECK(GetWide(kSource) == u"copy me");
}

REX_TEST_CASE("CRT wcsncpy pads with zeros and stops at the count", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abc");
  CHECK(CallAddress(rexshim_wcsncpy, {kDestination, kSource, 6}) == kDestination);
  CHECK(GetBytes(kDestination, 12) == std::string("\0a\0b\0c\0\0\0\0\0\0", 12));
  CHECK(IsUntouched(kDestination + 12, 8));
  ResetGuest();
  PutWide(kSource, u"abc");
  CHECK(CallAddress(rexshim_wcsncpy, {kDestination, kSource, 3}) == kDestination);
  CHECK(GetBytes(kDestination, 6) == std::string("\0a\0b\0c", 6));
  CHECK(IsUntouched(kDestination + 6, 8));
  ResetGuest();
  PutWide(kSource, u"abc");
  CHECK(CallAddress(rexshim_wcsncpy, {kDestination, kSource, 2}) == kDestination);
  CHECK(GetBytes(kDestination, 4) == std::string("\0a\0b", 4));
  CHECK(IsUntouched(kDestination + 4, 8));
}

REX_TEST_CASE("CRT wcsncpy_s copies within the destination size", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcd");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 8, kSource, 4}) == 0);
  CHECK(GetWide(kDestination) == u"abcd");
  CHECK(CallInt(rexshim_wcsncpy_s, {0, 8, kSource, 4}) == 22);
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 0, kSource, 4}) == 22);
}

REX_TEST_CASE("CRT wcsncpy_s clears the first unit when the destination is too small",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcd");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 3, kSource, 4}) == 34);
  CHECK(GetWide(kDestination).empty());
}

REX_TEST_CASE("CRT wcsncpy_s with _TRUNCATE copies what fits and returns 80",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcd");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 3, kSource, 0xFFFFFFFFu}) == 80);
  CHECK(GetWide(kDestination) == u"ab");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 8, kSource, 0xFFFFFFFFu}) == 0);
  CHECK(GetWide(kDestination) == u"abcd");
}

REX_TEST_CASE("CRT wcsncpy_s treats a count of zero and a null source as the CRT does",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kDestination, u"xyz");
  CHECK(CallInt(rexshim_wcsncpy_s, {0, 0, 0, 0}) == 0);
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 8, 0, 0}) == 0);
  CHECK(GetWide(kDestination).empty());
  PutWide(kDestination, u"xyz");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 8, 0, 4}) == 22);
  CHECK(GetWide(kDestination).empty());
}

REX_TEST_CASE("CRT wcsncpy_s stops at the count and terminates", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abcd");
  CHECK(CallInt(rexshim_wcsncpy_s, {kDestination, 8, kSource, 2}) == 0);
  CHECK(GetWide(kDestination) == u"ab");
}

REX_TEST_CASE("CRT wcscoll orders like wcscmp and reports a null argument",
              "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"abc");
  PutWide(kDestination, u"abd");
  CHECK(CallInt(rexshim_wcscoll, {kSource, kDestination}) == -1);
  CHECK(CallInt(rexshim_wcscoll, {kDestination, kSource}) == 1);
  CHECK(CallInt(rexshim_wcscoll, {kSource, kSource}) == 0);
  CHECK(CallInt(rexshim_wcscoll, {0, kSource}) == 0x7FFFFFFF);
  CHECK(CallInt(rexshim_wcscoll, {kSource, 0}) == 0x7FFFFFFF);
}

REX_TEST_CASE("CRT wcsstr finds a substring by big endian units", "[kernel][shim][crt][wide]") {
  ResetGuest();
  PutWide(kSource, u"hello world");
  PutWide(kDestination, u"wor");
  CHECK(CallAddress(rexshim_wcsstr, {kSource, kDestination}) == kSource + 12);
  PutWide(kDestination, u"xyz");
  CHECK(CallAddress(rexshim_wcsstr, {kSource, kDestination}) == 0);
  PutWide(kDestination, u"");
  CHECK(CallAddress(rexshim_wcsstr, {kSource, kDestination}) == kSource);
}
