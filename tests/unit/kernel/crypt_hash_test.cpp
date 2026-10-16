/**
 * @file        tests/unit/kernel/crypt_hash_test.cpp
 * @brief       The hash and HMAC exports match published and reference digests
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

constexpr u8 kSha1Abc[] = {0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
                           0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D};
constexpr u8 kSha1Nist2[] = {0x84, 0x98, 0x3E, 0x44, 0x1C, 0x3B, 0xD2, 0x6E, 0xBA, 0xAE,
                             0x4A, 0xA1, 0xF9, 0x51, 0x29, 0xE5, 0xE5, 0x46, 0x70, 0xF1};
constexpr u8 kSha1Pattern[] = {0xC9, 0xC9, 0x60, 0xA0, 0xB9, 0x25, 0x47, 0x4F, 0xAB, 0x83,
                               0x94, 0x2C, 0xC2, 0x7D, 0x50, 0x4F, 0xC2, 0x4A, 0xC3, 0x7B};
constexpr u8 kSha1Empty[] = {0xDA, 0x39, 0xA3, 0xEE, 0x5E, 0x6B, 0x4B, 0x0D, 0x32, 0x55,
                             0xBF, 0xEF, 0x95, 0x60, 0x18, 0x90, 0xAF, 0xD8, 0x07, 0x09};
constexpr u8 kSha256Abc[] = {0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40,
                             0xDE, 0x5D, 0xAE, 0x22, 0x23, 0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17,
                             0x7A, 0x9C, 0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD};
constexpr u8 kSha256Nist2[] = {0x24, 0x8D, 0x6A, 0x61, 0xD2, 0x06, 0x38, 0xB8, 0xE5, 0xC0, 0x26,
                               0x93, 0x0C, 0x3E, 0x60, 0x39, 0xA3, 0x3C, 0xE4, 0x59, 0x64, 0xFF,
                               0x21, 0x67, 0xF6, 0xEC, 0xED, 0xD4, 0x19, 0xDB, 0x06, 0xC1};
constexpr u8 kSha256Pattern[] = {0x4E, 0x4C, 0x29, 0x4B, 0x33, 0x1F, 0x7A, 0x20, 0x99, 0xA3, 0x79,
                                 0xBE, 0xC3, 0x4B, 0x9F, 0x9F, 0xC0, 0x3D, 0xC4, 0x6A, 0xB4, 0x65,
                                 0xD9, 0x98, 0xF4, 0xD6, 0x83, 0xDA, 0x53, 0x48, 0x7E, 0x6D};
constexpr u8 kSha256Empty[] = {0xE3, 0xB0, 0xC4, 0x42, 0x98, 0xFC, 0x1C, 0x14, 0x9A, 0xFB, 0xF4,
                               0xC8, 0x99, 0x6F, 0xB9, 0x24, 0x27, 0xAE, 0x41, 0xE4, 0x64, 0x9B,
                               0x93, 0x4C, 0xA4, 0x95, 0x99, 0x1B, 0x78, 0x52, 0xB8, 0x55};
constexpr u8 kSha384Abc[] = {
    0xCB, 0x00, 0x75, 0x3F, 0x45, 0xA3, 0x5E, 0x8B, 0xB5, 0xA0, 0x3D, 0x69, 0x9A, 0xC6, 0x50, 0x07,
    0x27, 0x2C, 0x32, 0xAB, 0x0E, 0xDE, 0xD1, 0x63, 0x1A, 0x8B, 0x60, 0x5A, 0x43, 0xFF, 0x5B, 0xED,
    0x80, 0x86, 0x07, 0x2B, 0xA1, 0xE7, 0xCC, 0x23, 0x58, 0xBA, 0xEC, 0xA1, 0x34, 0xC8, 0x25, 0xA7};
constexpr u8 kSha384Nist2[] = {
    0x33, 0x91, 0xFD, 0xDD, 0xFC, 0x8D, 0xC7, 0x39, 0x37, 0x07, 0xA6, 0x5B, 0x1B, 0x47, 0x09, 0x39,
    0x7C, 0xF8, 0xB1, 0xD1, 0x62, 0xAF, 0x05, 0xAB, 0xFE, 0x8F, 0x45, 0x0D, 0xE5, 0xF3, 0x6B, 0xC6,
    0xB0, 0x45, 0x5A, 0x85, 0x20, 0xBC, 0x4E, 0x6F, 0x5F, 0xE9, 0x5B, 0x1F, 0xE3, 0xC8, 0x45, 0x2B};
constexpr u8 kSha384Pattern[] = {
    0x7A, 0x2F, 0x8C, 0x7F, 0x12, 0x34, 0x49, 0x64, 0xA1, 0x3C, 0xB9, 0x26, 0x04, 0x92, 0xB8, 0x45,
    0xE5, 0x66, 0x15, 0xD6, 0x15, 0x2B, 0x9E, 0xB9, 0xE5, 0x4B, 0x58, 0x0F, 0xC8, 0x84, 0x05, 0xE6,
    0x4F, 0x31, 0x81, 0x3B, 0xFD, 0xA1, 0x0D, 0xE2, 0xA6, 0x42, 0xFD, 0xF1, 0x67, 0x6C, 0x61, 0xB4};
constexpr u8 kSha384Empty[] = {
    0x38, 0xB0, 0x60, 0xA7, 0x51, 0xAC, 0x96, 0x38, 0x4C, 0xD9, 0x32, 0x7E, 0xB1, 0xB1, 0xE3, 0x6A,
    0x21, 0xFD, 0xB7, 0x11, 0x14, 0xBE, 0x07, 0x43, 0x4C, 0x0C, 0xC7, 0xBF, 0x63, 0xF6, 0xE1, 0xDA,
    0x27, 0x4E, 0xDE, 0xBF, 0xE7, 0x6F, 0x65, 0xFB, 0xD5, 0x1A, 0xD2, 0xF1, 0x48, 0x98, 0xB9, 0x5B};
constexpr u8 kSha512Abc[] = {
    0xDD, 0xAF, 0x35, 0xA1, 0x93, 0x61, 0x7A, 0xBA, 0xCC, 0x41, 0x73, 0x49, 0xAE, 0x20, 0x41, 0x31,
    0x12, 0xE6, 0xFA, 0x4E, 0x89, 0xA9, 0x7E, 0xA2, 0x0A, 0x9E, 0xEE, 0xE6, 0x4B, 0x55, 0xD3, 0x9A,
    0x21, 0x92, 0x99, 0x2A, 0x27, 0x4F, 0xC1, 0xA8, 0x36, 0xBA, 0x3C, 0x23, 0xA3, 0xFE, 0xEB, 0xBD,
    0x45, 0x4D, 0x44, 0x23, 0x64, 0x3C, 0xE8, 0x0E, 0x2A, 0x9A, 0xC9, 0x4F, 0xA5, 0x4C, 0xA4, 0x9F};
constexpr u8 kSha512Nist2[] = {
    0x20, 0x4A, 0x8F, 0xC6, 0xDD, 0xA8, 0x2F, 0x0A, 0x0C, 0xED, 0x7B, 0xEB, 0x8E, 0x08, 0xA4, 0x16,
    0x57, 0xC1, 0x6E, 0xF4, 0x68, 0xB2, 0x28, 0xA8, 0x27, 0x9B, 0xE3, 0x31, 0xA7, 0x03, 0xC3, 0x35,
    0x96, 0xFD, 0x15, 0xC1, 0x3B, 0x1B, 0x07, 0xF9, 0xAA, 0x1D, 0x3B, 0xEA, 0x57, 0x78, 0x9C, 0xA0,
    0x31, 0xAD, 0x85, 0xC7, 0xA7, 0x1D, 0xD7, 0x03, 0x54, 0xEC, 0x63, 0x12, 0x38, 0xCA, 0x34, 0x45};
constexpr u8 kSha512Pattern[] = {
    0x50, 0x96, 0x49, 0x8D, 0x96, 0xF5, 0x0F, 0x9A, 0x13, 0x7C, 0x4D, 0xB5, 0xB8, 0xB0, 0xCD, 0x38,
    0x38, 0x3A, 0xD5, 0x53, 0x50, 0xFB, 0x5A, 0x98, 0x80, 0x5F, 0xED, 0xC3, 0x1F, 0xA1, 0x26, 0x2F,
    0x1F, 0x0C, 0xF4, 0xD6, 0xF1, 0x2D, 0x7E, 0xCD, 0x8D, 0xED, 0xD9, 0x33, 0xA4, 0xC9, 0x12, 0x63,
    0x44, 0xFE, 0x22, 0xE9, 0x37, 0xA8, 0xAD, 0x35, 0xFD, 0xEA, 0xE1, 0xE8, 0x76, 0xAE, 0x69, 0x8B};
constexpr u8 kSha512Empty[] = {
    0xCF, 0x83, 0xE1, 0x35, 0x7E, 0xEF, 0xB8, 0xBD, 0xF1, 0x54, 0x28, 0x50, 0xD6, 0x6D, 0x80, 0x07,
    0xD6, 0x20, 0xE4, 0x05, 0x0B, 0x57, 0x15, 0xDC, 0x83, 0xF4, 0xA9, 0x21, 0xD3, 0x6C, 0xE9, 0xCE,
    0x47, 0xD0, 0xD1, 0x3C, 0x5D, 0x85, 0xF2, 0xB0, 0xFF, 0x83, 0x18, 0xD2, 0x87, 0x7E, 0xEC, 0x2F,
    0x63, 0xB9, 0x31, 0xBD, 0x47, 0x41, 0x7A, 0x81, 0xA5, 0x38, 0x32, 0x7A, 0xF9, 0x27, 0xDA, 0x3E};
constexpr u8 kHmacSha1Jefe[] = {0xEF, 0xFC, 0xDF, 0x6A, 0xE5, 0xEB, 0x2F, 0xA2, 0xD2, 0x74,
                                0x16, 0xD5, 0xF1, 0x84, 0xDF, 0x9C, 0x25, 0x9A, 0x7C, 0x79};
constexpr u8 kHmacSha1Hi[] = {0xB6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xE2, 0x8B,
                              0xC0, 0xB6, 0xFB, 0x37, 0x8C, 0x8E, 0xF1, 0x46, 0xBE, 0x00};
constexpr u8 kHmacSha1Long[] = {0x07, 0x0A, 0x98, 0x99, 0x2C, 0x4C, 0x1A, 0x83, 0x47, 0x4C,
                                0xB7, 0x80, 0xFC, 0x56, 0x46, 0x08, 0xDF, 0x3C, 0xF5, 0x03};

std::vector<u8> Pattern() {
  std::vector<u8> bytes(1000);
  for (u32 i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<u8>(i % 251);
  }
  return bytes;
}

std::vector<u8> Text(const char* text) {
  return std::vector<u8>(text, text + std::strlen(text));
}

u32 Put(Caller& c, std::span<const u8> bytes) {
  const u32 address = c.Alloc(static_cast<u32>(bytes.size()) + 8);
  for (u32 offset = 0; offset < bytes.size(); offset += 4) {
    u8 word[4]{};
    std::memcpy(word, bytes.data() + offset, std::min<size_t>(4, bytes.size() - offset));
    c.Store32(address + offset,
              (u32{word[0]} << 24) | (u32{word[1]} << 16) | (u32{word[2]} << 8) | word[3]);
  }
  return address;
}

std::vector<u8> Get(Caller& c, u32 address, u32 size) {
  std::vector<u8> bytes(size);
  for (u32 i = 0; i < size; ++i) {
    bytes[i] = c.Load8(address + i);
  }
  return bytes;
}

std::vector<u8> Expected(std::span<const u8> digest, size_t size = SIZE_MAX) {
  const std::span<const u8> head = digest.first(std::min(size, digest.size()));
  return std::vector<u8>(head.begin(), head.end());
}

struct Message {
  const char* name;
  std::vector<u8> bytes;
};

struct Algorithm {
  const char* name;
  const char* init;
  const char* update;
  const char* final;
  const char* one_shot;
  u32 digest_size;
  std::array<std::span<const u8>, 4> digests;
};

std::vector<Message> Messages() {
  return {{"abc", Text("abc")},
          {"two blocks", Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")},
          {"pattern", Pattern()},
          {"empty", {}}};
}

std::vector<Algorithm> Algorithms() {
  return {
      {"SHA-1",
       "XeCryptShaInit",
       "XeCryptShaUpdate",
       "XeCryptShaFinal",
       "XeCryptSha",
       20,
       {kSha1Abc, kSha1Nist2, kSha1Pattern, kSha1Empty}},
      {"SHA-256",
       "XeCryptSha256Init",
       "XeCryptSha256Update",
       "XeCryptSha256Final",
       "XeCryptSha256",
       32,
       {kSha256Abc, kSha256Nist2, kSha256Pattern, kSha256Empty}},
      {"SHA-384",
       "XeCryptSha384Init",
       "XeCryptSha384Update",
       "XeCryptSha384Final",
       "XeCryptSha384",
       48,
       {kSha384Abc, kSha384Nist2, kSha384Pattern, kSha384Empty}},
      {"SHA-512",
       "XeCryptSha512Init",
       "XeCryptSha512Update",
       "XeCryptSha512Final",
       "XeCryptSha512",
       64,
       {kSha512Abc, kSha512Nist2, kSha512Pattern, kSha512Empty}},
  };
}

std::vector<u8> RunIncremental(Caller& c, const Algorithm& algorithm,
                               const std::vector<u8>& message, u32 out_size) {
  const u32 state = c.Alloc(0x100);
  const u32 out = c.Alloc(0x80);
  c.Kernel(algorithm.init, {state});
  const std::array<size_t, 3> cuts = {1, 63, message.size()};
  size_t offset = 0;
  for (size_t cut : cuts) {
    const size_t end = std::min(message.size(), offset + cut);
    const std::span<const u8> part(message.data() + offset, end - offset);
    c.Kernel(algorithm.update, {state, Put(c, part), part.size()});
    offset = end;
  }
  c.Kernel(algorithm.final, {state, out, out_size});
  return Get(c, out, out_size);
}

struct HmacCase {
  std::vector<u8> key;
  std::vector<u8> message;
  std::span<const u8> sha;
};

std::vector<HmacCase> HmacCases() {
  return {{Text("Jefe"), Text("what do ya want for nothing?"), kHmacSha1Jefe},
          {std::vector<u8>(20, 0x0B), Text("Hi There"), kHmacSha1Hi},
          {std::vector<u8>(80, 0xAA),
           Text("Test Using Larger Than Block-Size Key - Hash Key First"), kHmacSha1Long}};
}

}  // namespace

REX_TEST_CASE("Incremental hashing matches the reference digests", "[kernel][crypt][hash]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const Algorithm& algorithm : Algorithms()) {
      const auto messages = Messages();
      for (size_t i = 0; i < messages.size(); ++i) {
        INFO(algorithm.name << " " << messages[i].name);
        CHECK(RunIncremental(c, algorithm, messages[i].bytes, algorithm.digest_size) ==
              Expected(algorithm.digests[i]));
      }
    }
  });
}

REX_TEST_CASE("A short output size returns a digest prefix", "[kernel][crypt][hash]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const Algorithm& algorithm : Algorithms()) {
      INFO(algorithm.name);
      CHECK(RunIncremental(c, algorithm, Text("abc"), 7) == Expected(algorithm.digests[0], 7));
    }
  });
}

REX_TEST_CASE("One shot hashing concatenates up to 3 inputs", "[kernel][crypt][hash]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> pattern = Pattern();
    for (const Algorithm& algorithm : Algorithms()) {
      INFO(algorithm.name);
      const u32 out = c.Alloc(0x80);
      const u32 a = Put(c, std::span<const u8>(pattern.data(), 100));
      const u32 b = Put(c, std::span<const u8>(pattern.data() + 100, 300));
      const u32 d = Put(c, std::span<const u8>(pattern.data() + 400, 600));
      c.Kernel(algorithm.one_shot, {a, 100, b, 300, d, 600, out, algorithm.digest_size});
      CHECK(Get(c, out, algorithm.digest_size) == Expected(algorithm.digests[2]));

      const u32 out_two = c.Alloc(0x80);
      c.Kernel(algorithm.one_shot,
               {Put(c, Text("abc")), 3, 0, 0, 0, 0, out_two, algorithm.digest_size});
      CHECK(Get(c, out_two, algorithm.digest_size) == Expected(algorithm.digests[0]));
    }
  });
}

REX_TEST_CASE("A hash state copied in the middle of a message continues independently",
              "[kernel][crypt][hash]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> pattern = Pattern();
    for (const Algorithm& algorithm : Algorithms()) {
      INFO(algorithm.name);
      const u32 state = c.Alloc(0x100);
      const u32 copy = c.Alloc(0x100);
      c.Kernel(algorithm.init, {state});
      c.Kernel(algorithm.update, {state, Put(c, std::span<const u8>(pattern.data(), 500)), 500});
      for (u32 offset = 0; offset < 0x100; offset += 4) {
        c.Store32(copy + offset, c.Load32(state + offset));
      }
      const u32 rest = Put(c, std::span<const u8>(pattern.data() + 500, 500));
      const u32 out_a = c.Alloc(0x80);
      const u32 out_b = c.Alloc(0x80);
      c.Kernel(algorithm.update, {state, rest, 500});
      c.Kernel(algorithm.final, {state, out_a, algorithm.digest_size});
      c.Kernel(algorithm.update, {copy, rest, 500});
      c.Kernel(algorithm.final, {copy, out_b, algorithm.digest_size});
      CHECK(Get(c, out_a, algorithm.digest_size) == Expected(algorithm.digests[2]));
      CHECK(Get(c, out_b, algorithm.digest_size) == Expected(algorithm.digests[2]));
    }
  });
}

REX_TEST_CASE("HMAC exports match the RFC 2202 digests", "[kernel][crypt][hash][hmac]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    constexpr u32 kDigestSize = 20;
    for (const HmacCase& item : HmacCases()) {
      {
        INFO("key size " << item.key.size());
        const auto expected = Expected(item.sha);
        const u32 key = Put(c, item.key);
        const u32 message = Put(c, item.message);
        const u32 out = c.Alloc(0x40);
        c.Kernel("XeCryptHmacSha", {key, item.key.size(), message, item.message.size(), 0, 0, 0, 0,
                                    out, kDigestSize});
        CHECK(Get(c, out, kDigestSize) == expected);

        const u32 state = c.Alloc(0x100);
        const u32 out_inc = c.Alloc(0x40);
        c.Kernel("XeCryptHmacShaInit", {state, key, item.key.size()});
        const size_t half = item.message.size() / 2;
        c.Kernel("XeCryptHmacShaUpdate",
                 {state, Put(c, std::span<const u8>(item.message.data(), half)), half});
        const size_t rest = item.message.size() - half;
        c.Kernel("XeCryptHmacShaUpdate",
                 {state, Put(c, std::span<const u8>(item.message.data() + half, rest)), rest});
        c.Kernel("XeCryptHmacShaFinal", {state, out_inc, kDigestSize});
        CHECK(Get(c, out_inc, kDigestSize) == expected);
      }
    }
  });
}

REX_TEST_CASE("HMAC SHA cuts a key past 64 bytes to 64 bytes", "[kernel][crypt][hash][hmac]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> key(80, 0xAA);
    const std::vector<u8> message = Text("Test Using Larger Than Block-Size Key - Hash Key First");
    const u32 message_address = Put(c, message);
    const u32 out_long = c.Alloc(0x40);
    const u32 out_cut = c.Alloc(0x40);
    c.Kernel("XeCryptHmacSha",
             {Put(c, key), 80, message_address, message.size(), 0, 0, 0, 0, out_long, 20});
    c.Kernel("XeCryptHmacSha",
             {Put(c, key), 64, message_address, message.size(), 0, 0, 0, 0, out_cut, 20});
    CHECK(Get(c, out_long, 20) == Get(c, out_cut, 20));
    constexpr u8 kCutDigest[] = {0x07, 0x0A, 0x98, 0x99, 0x2C, 0x4C, 0x1A, 0x83, 0x47, 0x4C,
                                 0xB7, 0x80, 0xFC, 0x56, 0x46, 0x08, 0xDF, 0x3C, 0xF5, 0x03};
    CHECK(Get(c, out_long, 20) == Expected(kCutDigest));

    const u32 state = c.Alloc(0x100);
    const u32 out_inc = c.Alloc(0x40);
    c.Kernel("XeCryptHmacShaInit", {state, Put(c, key), 80});
    c.Kernel("XeCryptHmacShaUpdate", {state, message_address, message.size()});
    c.Kernel("XeCryptHmacShaFinal", {state, out_inc, 20});
    CHECK(Get(c, out_inc, 20) == Get(c, out_cut, 20));
  });
}

REX_TEST_CASE("HMAC SHA verify reports a match with 1 and a mismatch with 0",
              "[kernel][crypt][hash][hmac]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const HmacCase item = HmacCases()[0];
    const u32 key = Put(c, item.key);
    const u32 message = Put(c, item.message);
    const u32 good = Put(c, item.sha);
    std::vector<u8> bad_bytes(item.sha.begin(), item.sha.end());
    bad_bytes[19] ^= 1;
    const u32 bad = Put(c, bad_bytes);
    const u32 size = static_cast<u32>(item.message.size());
    CHECK(c.Kernel("XeCryptHmacShaVerify", {key, 4, message, size, 0, 0, 0, 0, good, 20}) == 1);
    CHECK(c.Kernel("XeCryptHmacShaVerify", {key, 4, message, size, 0, 0, 0, 0, bad, 20}) == 0);
    CHECK(c.Kernel("XeCryptHmacShaVerify", {key, 4, message, size, 0, 0, 0, 0, good, 10}) == 1);
    CHECK(c.Kernel("XeCryptHmacShaVerify", {key, 4, message, size, 0, 0, 0, 0, good, 21}) == 0);
  });
}
