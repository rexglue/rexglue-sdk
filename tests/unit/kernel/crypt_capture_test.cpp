/**
 * @file        tests/unit/kernel/crypt_capture_test.cpp
 * @brief       The hash, cipher and state exports match the results measured on a console
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
#include <string>
#include <string_view>
#include <vector>

#include "kernel_fixture.h"

using rex::testing::Caller;
using rex::testing::KernelFixture;

namespace {

struct MeasuredCase {
  const char* name;
  const char* kind;
  const char* key;
  const char* first;
  const char* second;
  const char* third;
  const char* compare;
  u32 compare_size;
  u32 output_size;
  u32 r3;
  const char* output;
};

const MeasuredCase kMeasuredCases[] = {
    {"sha.3", "sha", "", "61", "62", "63", "", 0, 20, 0,
     "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {"sha.0", "sha", "", "", "", "", "", 0, 20, 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {"sha.56", "sha", "", "000102030405060708090a0b0c0d0e0f1011",
     "12131415161718191a1b1c1d1e1f20212223", "2425262728292a2b2c2d2e2f3031323334353637", "", 0, 20,
     0, "636e2ec698dac903498e648bd2f3af641d3c88cb"},
    {"hmacsha.1", "hmacsha", "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "4869205468657265", "", "",
     "", 0, 20, 0, "b617318655057264e28bc0b6fb378c8ef146be00"},
    {"hmacsha.2", "hmacsha", "4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
     "", "", "", 0, 20, 0, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"},
    {"hmacsha.3", "hmacsha", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
     "ddddddd",
     "", "", "", 0, 20, 0, "125d7342b9ac11cd91a39af48aa17b4f63f175d3"},
    {"hmacsha.4", "hmacsha", "0102030405060708090a0b0c0d0e0f10111213141516171819",
     "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdc"
     "dcdcdcd",
     "", "", "", 0, 20, 0, "4c9007f4026250c6bc8414f9bf50c86c2d7235da"},
    {"hmacsha.5", "hmacsha", "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c",
     "546573742057697468205472756e636174696f6e", "", "", "", 0, 20, 0,
     "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04"},
    {"hmacsha.6", "hmacsha",
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6"
     "579204669727374",
     "", "", "", 0, 20, 0, "070a98992c4c1a83474cb780fc564608df3cf503"},
    {"hmacsha.7", "hmacsha",
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b657920616e64204c6172676"
     "572205468616e204f6e6520426c6f636b2d53697a652044617461",
     "", "", "", 0, 20, 0, "a6aed3e73996c8be5fc7ba1260fd81172663ec65"},
    {"hmacsha.8", "hmacsha",
     "05121f2c394653606d7a8794a1aebbc8d5e2effc091623303d4a5764717e8b98a5b2bfccd9e6f3000d1a2734414e5"
     "b6875828f9ca9b6c3d0ddeaf704111e2b3845525f6c798693a0adbac7d4e1eefb0815222f3c495663707d8a97a4b1"
     "becbd8e5f2ff0c",
     "6c6f6e67206b6579206f766572203634206279746573", "", "", "", 0, 20, 0,
     "7625a20ed66c4ca5a3a4d747c26d87e518bb3301"},
    {"verify.0.match", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "ef5555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     0, 0, 1, ""},
    {"verify.0.mismatch", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "ee5555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     0, 0, 1, ""},
    {"verify.1.match", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "ef5555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     1, 0, 1, ""},
    {"verify.1.mismatch", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "ee5555555555555555555555555555555555555555555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     1, 0, 0, ""},
    {"verify.20.match", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "effcdf6ae5eb2fa2d27416d5f184df9c259a7c7955555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     20, 0, 1, ""},
    {"verify.20.mismatch", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "eefcdf6ae5eb2fa2d27416d5f184df9c259a7c7955555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     20, 0, 0, ""},
    {"verify.21.match", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "effcdf6ae5eb2fa2d27416d5f184df9c259a7c7955555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     21, 0, 0, ""},
    {"verify.21.mismatch", "shaverify", "4a656665",
     "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "", "",
     "eefcdf6ae5eb2fa2d27416d5f184df9c259a7c7955555555555555555555555555555555555555555555555555555"
     "55555555555555555555555555555555555",
     21, 0, 0, ""},
    {"sha256.3", "sha256", "", "616263", "", "", "", 0, 32, 0,
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"sha256.0", "sha256", "", "", "", "", "", 0, 32, 0,
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"sha256.56", "sha256", "",
     "6162636462636465636465666465666765666768666768696768696a68696a6b696a6b6c6a6b6c6d6b6c6d6e6c6d6"
     "e6f6d6e6f706e6f7071",
     "", "", "", 0, 32, 0, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"sha384.3", "sha384", "", "616263", "", "", "", 0, 48, 0,
     "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c82"
     "5a7"},
    {"sha512.3", "sha512", "", "616263", "", "", "", 0, 64, 0,
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3fee"
     "bbd454d4423643ce80e2a9ac94fa54ca49f"},
    {"sha384.0", "sha384", "", "", "", "", "", 0, 48, 0,
     "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b"
     "95b"},
    {"sha512.0", "sha512", "", "", "", "", "", 0, 64, 0,
     "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877ee"
     "c2f63b931bd47417a81a538327af927da3e"},
    {"sha384.56", "sha384", "",
     "6162636462636465636465666465666765666768666768696768696a68696a6b696a6b6c6a6b6c6d6b6c6d6e6c6d6"
     "e6f6d6e6f706e6f7071",
     "", "", "", 0, 48, 0,
     "3391fdddfc8dc7393707a65b1b4709397cf8b1d162af05abfe8f450de5f36bc6b0455a8520bc4e6f5fe95b1fe3c84"
     "52b"},
    {"sha512.56", "sha512", "",
     "6162636462636465636465666465666765666768666768696768696a68696a6b696a6b6c6a6b6c6d6b6c6d6e6c6d6"
     "e6f6d6e6f706e6f7071",
     "", "", "", 0, 64, 0,
     "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c33596fd15c13b1b07f9aa1d3bea57789"
     "ca031ad85c7a71dd70354ec631238ca3445"},
    {"sha384.112", "sha384", "",
     "61626364656667686263646566676869636465666768696a6465666768696a6b65666768696a6b6c666768696a6b6"
     "c6d6768696a6b6c6d6e68696a6b6c6d6e6f696a6b6c6d6e6f706a6b6c6d6e6f70716b6c6d6e6f7071726c6d6e6f70"
     "7172736d6e6f70717273746e6f707172737475",
     "", "", "", 0, 48, 0,
     "09330c33f71147e83d192fc782cd1b4753111b173b3b05d22fa08086e3b0f712fcc7c71a557e2db966c3e9fa91746"
     "039"},
    {"sha512.112", "sha512", "",
     "61626364656667686263646566676869636465666768696a6465666768696a6b65666768696a6b6c666768696a6b6"
     "c6d6768696a6b6c6d6e68696a6b6c6d6e6f696a6b6c6d6e6f706a6b6c6d6e6f70716b6c6d6e6f7071726c6d6e6f70"
     "7172736d6e6f70717273746e6f707172737475",
     "", "", "", 0, 64, 0,
     "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b54"
     "33ac7d329eeb6dd26545e96e55b874be909"},
    {"rotsum.a", "rotsum", "",
     "616263616263616263616263616263616263616263616263616263616263616263616263616263616263616263616"
     "263616263616263616263616263",
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f2021222324252627", "", "", 0,
     20, 0, "3d3c11a71e26e2e4bd56595682200cbed1c96b31"},
    {"rotsum.b", "rotsum", "",
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2"
     "e2f303132333435363738393a3b3c3d3e3f",
     "", "", "", 0, 20, 0, "fb6986a2b8f5d2403b0c92c00d9c9ec1148a9c0f"},
    {"rc4.5", "rc4", "0102030405",
     "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
     "000",
     "", "", "", 0, 48, 0,
     "b2396305f03dc027ccc3524a0a1118a86982944f18fc82d589c403a47a0d0919b68f78c28d15d822234c6cb8c5f6c"
     "4ac"},
    {"rc4.16", "rc4", "0102030405060708090a0b0c0d0e0f10",
     "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
     "000",
     "", "", "", 0, 48, 0,
     "9ac7cc9a609d1ef7b2932899cde41b975248c4959014126a6e8a84f11d1a9e1c0b9de3e45521a0bf5b9a607bf634c"
     "cf5"},
    {"rc4.32", "rc4", "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
     "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
     "000",
     "", "", "", 0, 48, 0,
     "eaa6bd25880bf93d3f5d1e4ca2611d91cfa45c9f7e714b54bdfa80027cb14380f4dd33bda70179711136b47a99fc9"
     "fce"},
    {"desparity.01", "desparity", "", "01", "", "", "", 0, 1, 0, "01"},
    {"desparity.03", "desparity", "", "03", "", "", "", 0, 1, 0, "02"},
    {"desparity.ff", "desparity", "", "ff", "", "", "", 0, 1, 0, "fe"},
    {"desparity.00", "desparity", "", "00", "", "", "", 0, 1, 0, "01"},
    {"desparity.all", "desparity", "",
     "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2"
     "e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c"
     "5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8"
     "b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9"
     "babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e"
     "8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
     "", "", "", 0, 256, 0,
     "010102020404070708080b0b0d0d0e0e101013131515161619191a1a1c1c1f1f202023232525262629292a2a2c2c2"
     "f2f313132323434373738383b3b3d3d3e3e404043434545464649494a4a4c4c4f4f515152525454575758585b5b5d"
     "5d5e5e616162626464676768686b6b6d6d6e6e707073737575767679797a7a7c7c7f7f808083838585868689898a8"
     "a8c8c8f8f919192929494979798989b9b9d9d9e9ea1a1a2a2a4a4a7a7a8a8ababadadaeaeb0b0b3b3b5b5b6b6b9b9"
     "bababcbcbfbfc1c1c2c2c4c4c7c7c8c8cbcbcdcdceced0d0d3d3d5d5d6d6d9d9dadadcdcdfdfe0e0e3e3e5e5e6e6e"
     "9e9eaeaececefeff1f1f2f2f4f4f7f7f8f8fbfbfdfdfefe"},
};

std::vector<u8> Hex(std::string_view text) {
  std::vector<u8> bytes(text.size() / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<u8>(std::stoi(std::string(text.substr(i * 2, 2)), nullptr, 16));
  }
  return bytes;
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

u32 PutFilled(Caller& c, u32 size) {
  std::vector<u8> bytes(size);
  for (u32 i = 0; i < size; ++i) {
    bytes[i] = std::array<u8, 4>{0xDE, 0xAD, 0xBE, 0xEF}[i % 4];
  }
  return Put(c, bytes);
}

const char* HashExport(std::string_view kind) {
  if (kind == "sha") {
    return "XeCryptSha";
  }
  if (kind == "sha256") {
    return "XeCryptSha256";
  }
  if (kind == "sha384") {
    return "XeCryptSha384";
  }
  return "XeCryptSha512";
}

void RunMeasuredCase(Caller& c, const MeasuredCase& item) {
  INFO(item.name);
  const std::string_view kind = item.kind;
  const std::vector<u8> first = Hex(item.first);
  const std::vector<u8> second = Hex(item.second);
  const std::vector<u8> third = Hex(item.third);
  const std::vector<u8> key = Hex(item.key);
  u32 output = 0;
  if (kind == "sha" || kind == "sha256" || kind == "sha384" || kind == "sha512") {
    output = PutFilled(c, item.output_size);
    c.Kernel(HashExport(kind), {Put(c, first), first.size(), Put(c, second), second.size(),
                                Put(c, third), third.size(), output, item.output_size});
  } else if (kind == "hmacsha") {
    output = PutFilled(c, item.output_size);
    c.Kernel("XeCryptHmacSha",
             {Put(c, key), key.size(), Put(c, first), first.size(), Put(c, second), second.size(),
              Put(c, third), third.size(), output, item.output_size});
  } else if (kind == "shaverify") {
    CHECK(c.Kernel("XeCryptHmacShaVerify",
                   {Put(c, key), key.size(), Put(c, first), first.size(), Put(c, second),
                    second.size(), Put(c, third), third.size(), Put(c, Hex(item.compare)),
                    item.compare_size}) == item.r3);
    return;
  } else if (kind == "rotsum") {
    output = PutFilled(c, item.output_size);
    c.Kernel("XeCryptRotSumSha", {Put(c, first), first.size(), Put(c, second), second.size(),
                                  output, item.output_size});
  } else if (kind == "rc4") {
    output = Put(c, first);
    c.Kernel("XeCryptRc4", {Put(c, key), key.size(), output, first.size()});
  } else if (kind == "desparity") {
    output = PutFilled(c, item.output_size);
    c.Kernel("XeCryptDesParity", {Put(c, first), first.size(), output});
  }
  CHECK(Get(c, output, item.output_size) == Hex(item.output));
}

std::string ToHex(const std::vector<u8>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const u8 byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

void CheckState(Caller& c, u32 state, const char* expected) {
  CHECK(ToHex(Get(c, state, 0x180)) == std::string(expected));
}

}  // namespace

REX_TEST_CASE("The hash, HMAC, RotSumSha, RC4 and DES parity exports match the measured results",
              "[kernel][crypt][measured]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const MeasuredCase& item : kMeasuredCases) {
      RunMeasuredCase(c, item);
    }
  });
}

REX_TEST_CASE("The hash states match the measured layouts", "[kernel][crypt][measured]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> abc = {'a', 'b', 'c'};
    const std::vector<u8> hmac_key(16, 0x0B);
    struct HashState {
      const char* init;
      const char* update;
      const char* after_init;
      const char* after_update;
    };
    const HashState states[] = {
        {"XeCryptShaInit", "XeCryptShaUpdate",
         "0000000067452301efcdab8998badcfe10325476c3d2e1f0deadbeefdeadbeefdeadbeefdeadbeefdeadbeefd"
         "eadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
         "0000000367452301efcdab8998badcfe10325476c3d2e1f0616263efdeadbeefdeadbeefdeadbeefdeadbeefd"
         "eadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
        {"XeCryptSha256Init", "XeCryptSha256Update",
         "000000006a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19deadbeefdeadbeefd"
         "eadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
         "000000036a09e667bb67ae853c6ef372a54ff53a510e527f9b05688c1f83d9ab5be0cd19616263efdeadbeefd"
         "eadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
        {"XeCryptSha384Init", "XeCryptSha384Update",
         "00000000deadbeefcbbb9d5dc1059ed8629a292a367cd5079159015a3070dd17152fecd8f70e593967332667f"
         "fc00b318eb44a8768581511db0c2e0d64f98fa747b5481dbefa4fa4deadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
         "00000003deadbeefcbbb9d5dc1059ed8629a292a367cd5079159015a3070dd17152fecd8f70e593967332667f"
         "fc00b318eb44a8768581511db0c2e0d64f98fa747b5481dbefa4fa4616263efdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
        {"XeCryptSha512Init", "XeCryptSha512Update",
         "00000000deadbeef6a09e667f3bcc908bb67ae8584caa73b3c6ef372fe94f82ba54ff53a5f1d36f1510e527fa"
         "de682d19b05688c2b3e6c1f1f83d9abfb41bd6b5be0cd19137e2179deadbeefdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef",
         "00000003deadbeef6a09e667f3bcc908bb67ae8584caa73b3c6ef372fe94f82ba54ff53a5f1d36f1510e527fa"
         "de682d19b05688c2b3e6c1f1f83d9abfb41bd6b5be0cd19137e2179616263efdeadbeefdeadbeefdeadbeefde"
         "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdea"
         "dbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
         "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadb"
         "eefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
         "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbee"
         "fdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
         "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"},
    };
    for (const HashState& item : states) {
      INFO(item.init);
      const u32 state = PutFilled(c, 0x200);
      c.Kernel(item.init, {state});
      CheckState(c, state, item.after_init);
      c.Kernel(item.update, {state, Put(c, abc), 3});
      CheckState(c, state, item.after_update);
    }
    const u32 hmac = PutFilled(c, 0x200);
    c.Kernel("XeCryptHmacShaInit", {hmac, Put(c, hmac_key), 16});
    CheckState(
        c, hmac,
        "00000040498e15356c9bd198dd01df3bcc3c0a41ce69e4e7deadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
        "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef0000"
        "004053437a0177de5b5e326064462a132365bf8674afdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
        "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
        "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    c.Kernel("XeCryptHmacShaUpdate", {hmac, Put(c, abc), 3});
    CheckState(
        c, hmac,
        "00000043498e15356c9bd198dd01df3bcc3c0a41ce69e4e7616263efdeadbeefdeadbeefdeadbeefdeadbeefde"
        "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef0000"
        "004053437a0177de5b5e326064462a132365bf8674afdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
        "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
        "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
  });
}

REX_TEST_CASE("The cipher key states match the measured layouts", "[kernel][crypt][measured]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const u32 rc4_five = PutFilled(c, 0x200);
    c.Kernel("XeCryptRc4Key", {rc4_five, Put(c, Hex("0102030405")), 5});
    CheckState(
        c, rc4_five,
        "010308c9151b2343f291cf595c6d1f9007b637f1e0ffec532799dc8ca3ceed80602e631aaad2de69eb00a84410"
        "d65e0578cbfe930b87c2fa1d70aeb1b9db3477663c6f57fd298ed5318a362cf96462f04e745b1761c6e9b59faf"
        "4c063d897cca4f39328f549556049428a9229aee8dc7abb37667359ea025b802b0dda47aad12c4bc0ec0736c3f"
        "5172bb75d9df24a77d3b419d655d97262b7e821e4b4dd30db4c3683355407fbfd04a7b8ba1f7215038a5e6e1b7"
        "4530f8ea98719cda9bfce5efe31152e7cd8192861c0cbe42b2e8c8f56e6b0facf383a27946f4146a84471988d4"
        "f62f962d5f49203ea6d73a1813bd8548e42a0ad85ad116fbe2ccc5c109ba580000beefdeadbeefdeadbeefdead"
        "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

    const u32 rc4_sixteen = PutFilled(c, 0x200);
    c.Kernel("XeCryptRc4Key", {rc4_sixteen, Put(c, Hex("0102030405060708090a0b0c0d0e0f10")), 16});
    CheckState(
        c, rc4_sixteen,
        "01032165554b2d9447bd6c8644bfa258fb7e2324535da89960516a29ab7d77d9a78027386367596b1b98c51ee3"
        "3783faa4d7044376b1eef71c356d81734a269c15028c3fd8579fedc9161710f6ac3bf5622842c6fc40c270ea7c"
        "06f86168339300eb1431f22bb6919aa188497b85ccf112a6be7279c80c822e8d25d01384cb7f1a46aa69c7cf0d"
        "1f97fddc78e25ba33eadc1b4ba39059b9e8f09efd4c3e708b250db11af1dc44ce1d12a8b52d5180ab5a919305c"
        "e5924d668a3adace485a36ae4fe8d3cae0f9a0cd227434de3dc00ef06f4e56b3e987ffb732b9e4a58e9dec4571"
        "646e5e2cd290b8bb5f3c89feddbc962fe6df41b075950bf3d65420f40f7a070000beefdeadbeefdeadbeefdead"
        "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
        "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

    const u32 des3 = PutFilled(c, 0x200);
    c.Kernel("XeCryptDes3Key",
             {des3, Put(c, Hex("0123456789abcdef23456789abcdef01456789abcdef0123"))});
    CheckState(
        c, des3,
        "6464904042ce40ca1490985846898586c4b4488884050b849094e4388184814ed8a004f005cc0704a8f0281001"
        "0784cac84048d8074a800668d804a80701c1c80490e40ccd494309ac183024c8ce078424c07c10c18c08858c88"
        "c0388a8a4c82c048c824c18083834c0470a80b86c20b584020b4c84a094a00742c4cce0c0c4400742c4cce0c0c"
        "446464904042ce40ca740040548243ca88202cf0a484c1400a701c142400c8ca413028b4c40c020c8f40b8d800"
        "484b0e43e408a8684c004a8d68d804a80701c1c8885440c40244c54d30cc1cf40b470044b8a4105842010c4194"
        "6c8c400903c900182c848cc70586011c703c900c804741584020b4c84a094a78402094c84a494800f4284cce0c"
        "0d4494909858428d8186c4a4409885050b849894e4308184814ad86004f005cc860428f02890030384cac84048"
        "c8064a8046e408a868cc08428c0490640cc5c94389ac18702488ce058420c05c30c18c0887cc88c0380a824c82"
        "c048c824c102830b4c0430a80bc6c20b1c503c980c804745");

    const u32 aes = PutFilled(c, 0x200);
    c.Kernel("XeCryptAesKey", {aes, Put(c, Hex("000102030405060708090a0b0c0d0e0f"))});
    CheckState(
        c, aes,
        "000102030405060708090a0b0c0d0e0fd6aa74fdd2af72fadaa678f1d6ab76feb692cf0b643dbdf1be9bc50068"
        "30b3feb6ff744ed2c2c9bf6c590cbf0469bf4147f7f7bc95353e03f96c32bcfd058dfd3caaa3e8a99f9deb50f3"
        "af57adf622aa5e390f7df7a69296a7553dc10aa31f6b14f9701ae35fe28c440adf4d4ea9c02647438735a41c65"
        "b9e016baf4aebf7ad2549932d1f08557681093ed9cbe2c974e13111d7fe3944a17f307a78b4d2b30c500010203"
        "0405060708090a0b0c0d0e0f8c56dff0825dd3f9805ad3fc8659d7fda0db02992286d160a2dc029c2485d561c7"
        "c6e391e54032f1479c306d6319e50ca8a2f5044de2c7f50a7ef798696712942ec410276326d7d26958204a003f"
        "32de72e3098d11c5de5f789dfe1578a2cccb8d82fc749c47222be4dadc3e9c7810f51362a4638f2586486bff5a"
        "76f7874a8313aa29be9c8faff6f770f58000f7bf0313111d7fe3944a17f307a78b4d2b30c5deadbeefdeadbeef"
        "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
  });
}
