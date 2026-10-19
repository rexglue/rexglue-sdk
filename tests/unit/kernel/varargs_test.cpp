/**
 * @file        tests/unit/kernel/varargs_test.cpp
 * @brief       Variadic arguments past r10 are read from the argument image slots
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <bit>
#include <cstring>
#include <string>

#include <rex/runtime.h>
#include <rex/runtime/xmemory.h>

#include "kernel_fixture.h"

using rex::test::Caller;
using rex::test::KernelFixture;

namespace {
constexpr u32 kBufferBytes = 256;

std::string ReadBuffer(Caller& c, u32 address) {
  return std::string(c.GetRuntime().GetMemory()->TranslateVirtual<const char*>(address));
}

constexpr u64 kD1 = 0x11223344;
constexpr u64 kD2 = 0x55667788;
constexpr u64 kD3 = 0x99AABBCC;
constexpr u64 kD4 = 0xDDEEFF01;
constexpr u64 kD5 = 0x13579BDF;
constexpr u64 kD6 = 0x2468ACE0;
constexpr u64 kD7 = 0xFEDCBA98;
constexpr u64 kD8 = 0x76543210;
constexpr u64 kD9 = 0xA1B2C3D4;
constexpr u64 kD10 = 0xE5F60718;
constexpr u64 kD11 = 0x293A4B5C;
constexpr u64 kD12 = 0x6D7E8F90;
constexpr u64 kU1 = 0xAABBCCDD11223344;
constexpr u64 kU2 = 0x99887766DDEEFF01;
constexpr u64 kU3 = 0x0102030405060708;
constexpr u64 kU4 = 0xF1E2D3C4B5A69788;
constexpr u64 kU5 = 0x8070605040302010;
constexpr u64 kU6 = 0x1F2E3D4C5B6A7988;
constexpr u64 kU7 = 0x0C0FFEE00BADF00D;
constexpr u64 kU8 = 0x7A6B5C4D3E2F1A0B;
constexpr u64 kU9 = 0xD1C2B3A495867768;
constexpr u64 kU10 = 0x3C4D5E6F78899AAB;

template <typename... Args>
std::string Print(Caller& c, const char* format, Args... args) {
  const u32 buffer = c.Alloc(kBufferBytes);
  c.Kernel("_snprintf", {buffer, kBufferBytes, c.String(format), static_cast<u64>(args)...});
  return ReadBuffer(c, buffer);
}
}  // namespace

REX_TEST_CASE("varargs: the 8th variadic argument reads the 9th slot", "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(kBufferBytes);
    const u32 format = c.String("%d %d %d %d %d %d %d %d");
    c.Kernel("sprintf", {buffer, format, 1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(ReadBuffer(c, buffer) == "1 2 3 4 5 6 7 8");
  });
}

REX_TEST_CASE("varargs: a 64 bit value in the 9th slot keeps its high word", "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 buffer = c.Alloc(kBufferBytes);
    const u32 format = c.String("%d %d %d %d %d %d %I64d");
    c.Kernel("sprintf", {buffer, format, 1, 2, 3, 4, 5, 6, 0x1122334455667788});
    CHECK(ReadBuffer(c, buffer) == "1 2 3 4 5 6 1234605616436508552");
  });
}

REX_TEST_CASE("varargs: _snprintf places 32 bit, 64 bit and double arguments like the console",
              "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5, kD6,
                kD7, kD8, kD9) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,2468ACE0,FEDCBA98,76543210,A1B2C3D4");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X", kD1, kD2, kD3,
                kD4, kD5, kD6, kD7, kD8, kD9, kD10, kD11, kD12) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,2468ACE0,FEDCBA98,76543210,A1B2C3D4,"
          "E5F60718,293A4B5C,6D7E8F90");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%I64X,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5, kU1,
                kD6, kD7, kD8) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,AABBCCDD11223344,2468ACE0,FEDCBA98,"
          "76543210");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%I64X,%08X,%I64X,%08X,%08X", kD1, kD2, kD3, kD4, kU1, kD5,
                kU2, kD6, kD7) ==
          "11223344,55667788,99AABBCC,DDEEFF01,AABBCCDD11223344,13579BDF,99887766DDEEFF01,"
          "2468ACE0,FEDCBA98");
    CHECK(Print(c, "%I64X,%I64X,%I64X,%I64X,%I64X,%I64X,%I64X,%I64X,%I64X,%I64X", kU1, kU2, kU3,
                kU4, kU5, kU6, kU7, kU8, kU9, kU10) ==
          "AABBCCDD11223344,99887766DDEEFF01,102030405060708,F1E2D3C4B5A69788,8070605040302010,"
          "1F2E3D4C5B6A7988,C0FFEE00BADF00D,7A6B5C4D3E2F1A0B,D1C2B3A495867768,3C4D5E6F78899AAB");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%f,%f,%08X,%f", kD1, kD2, kD3, kD4, kD5,
                std::bit_cast<u64>(1.5), std::bit_cast<u64>(-2.25), kD6,
                std::bit_cast<u64>(12345.625)) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,1.500000,-2.250000,2468ACE0,12345.625000");
    CHECK(Print(c, "%08X,%08X,%08X,%f,%08X,%08X,%f,%08X,%08X", kD1, kD2, kD3,
                std::bit_cast<u64>(0.125), kD4, kD5, std::bit_cast<u64>(-2.25), kD6, kD7) ==
          "11223344,55667788,99AABBCC,0.125000,DDEEFF01,13579BDF,-2.250000,2468ACE0,FEDCBA98");
    CHECK(Print(c, "%08X,%I64X,%08X,%f,%08X,%I64X,%08X,%08X,%f,%I64X,%08X,%08X", kD1, kU1, kD2,
                std::bit_cast<u64>(1.5), kD3, kU2, kD4, kD5, std::bit_cast<u64>(-2.25), kU3, kD6,
                kD7) ==
          "11223344,AABBCCDD11223344,55667788,1.500000,99AABBCC,99887766DDEEFF01,DDEEFF01,"
          "13579BDF,-2.250000,102030405060708,2468ACE0,FEDCBA98");
  });
}

REX_TEST_CASE("varargs: _snprintf %I64X of a 32 bit argument prints the whole slot",
              "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%I64X,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5,
                0x998877662468ACE0, kD7, kD8, kD9) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,998877662468ACE0,FEDCBA98,76543210,"
          "A1B2C3D4");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%I64X,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5,
                0x99887766FFFFFFFE, kD7, kD8, kD9) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,99887766FFFFFFFE,FEDCBA98,76543210,"
          "A1B2C3D4");
  });
}

REX_TEST_CASE("varargs: _snprintf %llX and %08X print the low word of a 64 bit slot",
              "[kernel][varargs]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%llX,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5, kU1,
                kD6, kD7, kD8) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,11223344,2468ACE0,FEDCBA98,76543210");
    CHECK(Print(c, "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X", kD1, kD2, kD3, kD4, kD5, kU1,
                kD6, kD7, kD8) ==
          "11223344,55667788,99AABBCC,DDEEFF01,13579BDF,11223344,2468ACE0,FEDCBA98,76543210");
  });
}
