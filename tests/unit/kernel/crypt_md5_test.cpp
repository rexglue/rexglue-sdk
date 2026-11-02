/**
 * @file        tests/unit/kernel/crypt_md5_test.cpp
 * @brief       The MD5 and HMAC-MD5 exports match the results measured on a console and hashlib
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

using rex::test::Caller;
using rex::test::CopyToGuest;
using rex::test::KernelFixture;
using rex::test::ParseHex;
using rex::test::PutFilled;
using rex::test::ReadBytes;
using rex::test::ToHex;

namespace {

struct MeasuredCase {
  const char* name;
  const char* key;
  const char* first;
  const char* rest;
  const char* output;
};

const MeasuredCase kMeasuredCases[] = {
    {"md5.0", "", "", "|", "d41d8cd98f00b204e9800998ecf8427e"},
    {"md5.3", "", "61", "62|63", "900150983cd24fb0d6963f7d28e17f72"},
    {"md5.56", "", "000102030405060708090a0b0c0d0e0f1011",
     "12131415161718191a1b1c1d1e1f20212223|2425262728292a2b2c2d2e2f3031323334353637",
     "51fdd1acda72405dfdfa03fcb85896d7"},
    {"hmacmd5.1", "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "4869205468657265", "|",
     "9294727a3638bb1c13f48ef8158bfc9d"},
    {"hmacmd5.2", "4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f", "|",
     "750c783e6ab0b503eaa86e310a5db738"},
    {"hmacmd5.3", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
     "ddddddd",
     "|", "56be34521d144c88dbb8c733f0e8b3f6"}};

const char* const kPatternDigests[] = {
    "d41d8cd98f00b204e9800998ecf8427e", "8666683506aacd900bbd5a74ac4edf68",
    "c950ee5a697ceb1f1feafbca1147e254", "c9aee4810523ef8658121b8d492c6b41",
    "12508583d13baad57b58e0726ea91e98", "e318134e0ab2b7ab76c3876039f1c956",
    "e9d72b73add17cc32f4cb3c8c979a7ae", "cba0d4629465fa7ec898b1c04cb03904",
    "dab7d36c74183625ebceb4b3107d7e43", "94b32a20105751663116174b88c2feeb",
    "de1951cfd0dbdefcba237c7fd286b9e0", "4e20fb9e1ef5f854da04bd344092b6b5",
    "f872e64a6563c1fbdd486519ceda7581", "6580730e887f62b12a485f4ff03350ed",
    "0d302a05ceff16256dd7a5bd9300abf1", "a102af85951589c6397769d540865d4a",
    "93498065c90c7511629efc2a676cf256", "4e208ba7eda62e0fa7ce53387b02236d",
    "b063b0549163d220de1edd8a23b7b054", "ccb02c714bea290cded09732177d60b1",
    "b8db27b9098b351f2c0e7cab19289ed9", "9d51fbb3d75b7cf2c04e89615deb3921",
    "fe2bfa4a5af2ff39513368bed61769ca", "340a4188399e519adea4e7edd04c5496",
    "e53d52b691185dac65b8b327aab3e6d1", "4d13bdcb07302dddddc7ebebe5d79212",
    "a95d97bd92e42b61b54636e1a99f4cf5", "4b0da2e00a8a878848e1e8b7cecb8928",
    "7cb900b7d359fbc78c012f4b6fae2dc7", "1dc74ac243c8e8f15ab4f808c3f3df42",
    "455a0b77b347d98d102aef299878eeb4", "92d60eb7b58a4d66ad90b7935bfb33d5",
    "37e883d2c833cc697211727308d724e7", "559f842ab304a042cfcd6fad8713c61a",
    "bb7e61e5edbf72ff13c9fa4cf39999ab", "2ec00911fe3c0df28aa439f0985648dc",
    "e98df37143bb29417de8cb9dc5a3af3b", "7e5fc19c3dd2eefa0c469d1266ad28b2",
    "4caf8c4a661fa2443851f9717e27bfb3", "8fc9ad3f85168a8a79f83b9bd58b4700",
    "37d232517ba04c6989f1d5cee71209a0", "226587391a23f0454409dfa5610a0612",
    "7e64c287a80f89d9a81f324b6c134551", "73defb1b45f61588f2b0f0118f1689df",
    "c85aa221217beedb0191eb8764398c0c", "a7970086cb1f04f00782175f87507d9d",
    "49a71debb41eeb0cc8e14deea9827011", "dc081b0003ee8addefa76b0c4f465c67",
    "5117f4269ca5456f8dd0e47ddd3e0832", "84082d3a501ab9084b27c42000a2ab2d",
    "e4533360af50a086d3dec70231442d2d", "d480ff0eb166e7f0ae0199e979c950aa",
    "9381ce04c4b7d19016af251a8034a15d", "c2322828c65bcd66ec9fa1e2f6e56b5b",
    "eb5acae65a800805a13648f23dea0327", "52c0e574e1198de5fe3f8f11440dcb1b",
    "46c9907fc908ee68b1e7b8e71286a518", "1c805dd236c35cab25fcb1bc73802c51",
    "bca54f0684135200890418305d71acac", "01344f4f53fcb77c6f85b7c808411ee8",
    "88cedcdc923e0dc7346f92cc4cee4179", "0fae1a833824da26a06af17d24b5e783",
    "8eece4c814dc5eb0d6722df349794fe0", "a62f6d59e837867693f042f5b8f5a236",
    "7160b8fb5e9e4023d549c3971fbaeead", "70bd662e7aefbda85a0f7244167b7897",
    "a7e438b4b549aeac81d58ce084a8cc82", "91bbae8b4023df9d1865084b12770b41",
    "c28d503f96863ccadd87b48683251719", "8db68bc8c28f7e611403d41803363731",
    "5c9fb9f228663ac48fc9d6d7dab17e42", "26e7e113db273c68b0fc3ce4660fc5e2",
    "f9019e69c0b288ca2b635cfa0de38932", "5e422c5ded3c8a8396dfbad7d51601c0",
    "31ba317e4f6e5232ba904d6bba662218", "394aece866fa562b3a41a8e2ddf95f06",
    "5a467d47a0008e0fe77b3f7bb253a5b6", "b1e0930eb117db227c0a6c26b8b8d7e0",
    "9c1563ad259fe66a027e326a2230ab45", "87db68ef4d37224dd5046e45c90c5225",
    "f5b2e38db29de5b31a296c40c74ec344", "28e7be2dcbe1dcff632ca0efbed25d77",
    "8f5e295eb32a428971373489fd344024", "2754ef51ebf880201a029a6e9db1c6d8",
    "8421086f83cccc468349d475d38cda0e", "4d1280f4e99de07e63be0e3bcf144bf9",
    "ef4084f1ca79ad69bd70fe495db9ac67", "aa8d862a6f1357c1042ae12aba2dfc39",
    "5b24ebccd1c2488249709661fe45bb3a", "550fc67477f8007d1d5b8e7638fdb9b5",
    "a1f38bc207e8b3e316ef8072d3a9248a", "2527ae8df6f82117cc25441523944bac",
    "990c88dfd119106f0bd06aa464b41aff", "8e7b3ba41fcb83e3c85a6d9910f52832",
    "e9a01abe4ccedb7089319eec7e9f016d", "a5fdaa14db8fc0b635dbab5c72e3c178",
    "d43267cd1eba7a9af38f0a4689c32f69", "a909d98112ba19ce7e61a6bfee384151",
    "48486757a618701bbe40c31991341658", "03d21478673a8cbc41cc175cf82f4942",
    "f85d474760cb48f459327ba2209c185d", "1a30d5aeaa6b1dfdd86c7a9326a3a5d1",
    "0617fe93a0083d59a32aa3dc527d11f1", "a196bddcad45f15e5b5ecc17715a38fd",
    "a55daca3482d9d27a7ad621b73060dd4", "6e6c42ffecf0185840ec8e0db22381cf",
    "76d55aad95f3e42ff4bd457c9dc9ac8f", "38aba8bd6468a7577ef401c722d59170",
    "3e3714728d6560752fa4266bf28857f2", "5ddb36773fe9a86bc0f590e88583b807",
    "0ccd30cfa455fae2b4a073b0a7fb9afe", "13f00cb15f0c3fe588d1c17cde14b38f",
    "5fe6a9f76b541ec645559589da9a088a", "d4be13fd3019b6e959e11ff06e5671ae",
    "0faf24d27038b7f40b10efdb45bba699", "7b2107ccf358fd50d04be7572cb2a031",
    "9da080404bb3ef7b13abc1595ca0556d", "2268de7b9d7e1f34ecfdf5dc3629159b",
    "f7ee9ac5422f3dde35ddca3d87bf365b", "e84905d4214f4d1ca56c2cdcc152b143",
    "e3eb5a6c8669ea01a8c185b8abc8a5dc", "82f02aff7c685d5ef5f6abdfc38fd04f",
    "3264d4667a3f9b010916fdae50703f40", "0134c9c6cc77c9406fee9f9ab05e226f",
    "30b6c4313c98897e504387b98777ac6d", "55580346bc5457bde04f67ce8a5b0df5",
    "5e1ada0df43aedbcf8b20751d806693e", "acce2474d6cc8302120d09c818d17ef7",
    "10b2da1a82f16d99a81a7203fe9f02cb", "03fefbbcebe2959cf2270241aafe0250",
    "4298951876af905376a025f7279651c2", "ac4f81cb9d470ac167db2297e32b8385",
    "87d79bdda8ddd601bb4aeee65a644b04", "af18de9aa5c9d150e84f77321b628ef2",
    "7e3b95bd2736f30c985c61e9c33d799e", "3f60e7e290ce1e941cfa3fd7fc031695",
    "cd62db9573f2e9e2dc335b7b91f4ccfa", "0014c7743d2ebc2cf8539344087678fb",
    "6291c70f7389c02871b7ad0647c4a3f7", "db98149bede1cfc9e0be968a7cdfcdb7",
    "9db6ad6d9929d63c8c5fbadd5a02f1de", "c68adc6b78c265ebf965adfe71f6f77d",
    "a96e6381bffcce008e72816c1e2ee291", "2f7b531ae2a3ff58936c601d0b5a3cd2",
    "00a6351f7b12333eacdd669528b87221", "d1dedaaf7588d23f338f0802a1a3dba2",
    "b3876024cde26cc200104f881789a517", "e083a8ad31e85e818063e2048c2bf30e",
    "0a5c6c511dd8aa0fb3ee7c1a327c51ba", "7eb2691a7a7e01d2af0e3c2f1e0a6fbd",
    "67b90e1e5204b9c03936b9de7170903a", "8d4e8257311342562110facaa70fca8d",
    "9353a298cb5cfd5ca781d83b3d0d2df4", "6af81278007592e28467de2591e2a16b",
    "dea629695397a12973483a6e8e0680fe", "eba8aafc93df4ed44771a7af05f35241",
    "d8b13c0c16225e52172123c842c82d08", "adb36fefe0a249465662bc9604191e15",
    "4473c73f23485cc97976ce3c207114f4", "b04a6e091d8c572a111e6015cbb07188",
    "e5efab1ff1cfbac7328d501790e143a4", "f4bbe2aa23d9357d734eb1e29ad97420",
    "57b5b8b99c93189956e561c749aa0291", "611f9506da2494b02d812c8b4293b442",
    "f46eaa7cb6056fafc091170ef18120be", "6de817bab3ee282f7be51ac2e335b18e",
    "fe6bc3fc746a8810aa5a9a3c836c0224", "b6156cce390fd2eae228143c43ab2ca9",
    "93d07c3b82853b5bd52fdfc722548e11", "81c1504180cd21f59e7fd97e9740ecb9",
    "b59f5d0f90ebe4728f3d42d7e0d01ce3", "58738e705b780065e60d9a2bbfc2dc49",
    "db4fbf0e669c4be1826b25c8b9d82357", "5820ff781608eb19c3185f52b824e9dc",
    "4cbc3faad69dc09ea68562c9f986e0d6", "46968b9160bf6a3d604c23ebdc2d8fd0",
    "65f64a16cad9dd4f6e9aba7d7b8daea0", "5cd3b0cbeca269631860b6d875d18d69",
    "1cf586a553202b798cccf189b4e041a5", "0777274fb3417f2c4ecea5b7ff2aab69",
    "d204b84a047f6acfd82c1e0d0e65a384", "074ccb7dcaef16df2fb44b736757578f",
    "a4c226caee90199057cacd59c76eb65b", "790cd20cda6fb6439d0acdd937a6d7a4",
    "bf0e7b86f1889fdfc697ed1bb8994e20", "9508850ff68bd5445cfe8a73985b500a",
    "ec8f307b718f09e0832d4327612328dd", "7d35a0c5de3a5b41d81f9ab860e54fa2",
    "9bcd00157d835ac5247165a53d8a2055", "1eb83c3752087c0a3f374aa380b55f19",
    "852e31857c8f06a77ccaa5dd5b064244", "32d9bde85e98d3611dfb15f0fd4dc8d1",
    "4cf6bd4bc52d3ca824bfbc469b75fa97", "f5c8d2af674765ec08fda1eaff55c27d",
    "035c3d6111d38d92e3c2c0a5adff1a01", "3e97496b088331fa4816294003e7693f",
    "581674e9a91016e6f5a3ab4b6a7ec71c", "5adebe1b86bd85018534c380761e9025",
    "64f24eb9f6bea78595afc00482da373c", "c2aade7f9cb08fec12e38add3a818a52",
    "4c79b81ac94bad7a875519ce6b964c66"};

constexpr const char* kMd5Init =
    "0000000067452301efcdab8998badcfe10325476deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeef";
constexpr const char* kMd5Update =
    "0000000367452301efcdab8998badcfe10325476616263efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeef";
constexpr const char* kHmacInit =
    "00000040038733be290c39ed65703e6f8d4c823edeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef00000040d82b3cf526ab"
    "9142e7acdd55451307dbdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeef";
constexpr const char* kHmacUpdate =
    "00000043038733be290c39ed65703e6f8d4c823e616263efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef00000040d82b3cf526ab"
    "9142e7acdd55451307dbdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbe"
    "efdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdead"
    "beefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefde"
    "adbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"
    "deadbeefdeadbeef";

std::vector<u8> Pattern(u32 size) {
  std::vector<u8> bytes(size);
  for (u32 i = 0; i < size; ++i) {
    bytes[i] = static_cast<u8>(i * 7 + 3);
  }
  return bytes;
}

std::string Md5Streaming(Caller& c, const std::vector<u8>& data, u32 chunk) {
  const u32 state = PutFilled(c, 0x200);
  c.Kernel("XeCryptMd5Init", {state});
  for (u32 offset = 0; offset < data.size(); offset += chunk) {
    const u32 size = std::min<u32>(chunk, static_cast<u32>(data.size()) - offset);
    c.Kernel("XeCryptMd5Update",
             {state, CopyToGuest(c, std::span<const u8>(data).subspan(offset, size)), size});
  }
  const u32 output = PutFilled(c, 16);
  c.Kernel("XeCryptMd5Final", {state, output, 16});
  return ToHex(ReadBytes(c, output, 16));
}

}  // namespace

REX_TEST_CASE("XeCryptMd5 matches hashlib for every size from 0 to 200", "[kernel][crypt][md5]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (u32 size = 0; size <= 200; ++size) {
      INFO(size);
      const std::vector<u8> data = Pattern(size);
      const u32 output = PutFilled(c, 16);
      c.Kernel("XeCryptMd5", {CopyToGuest(c, data), size, 0, 0, 0, 0, output, 16});
      CHECK(ToHex(ReadBytes(c, output, 16)) == kPatternDigests[size]);
    }
  });
}

REX_TEST_CASE("XeCryptMd5Final matches hashlib across the padding boundary",
              "[kernel][crypt][md5]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (u32 size = 50; size <= 70; ++size) {
      INFO(size);
      CHECK(Md5Streaming(c, Pattern(size), 1) == kPatternDigests[size]);
      CHECK(Md5Streaming(c, Pattern(size), 7) == kPatternDigests[size]);
    }
  });
}

REX_TEST_CASE("XeCryptMd5 and XeCryptHmacMd5 match the measured results", "[kernel][crypt][md5]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const MeasuredCase& item : kMeasuredCases) {
      INFO(item.name);
      const std::vector<u8> key = ParseHex(item.key);
      const std::vector<u8> first = ParseHex(item.first);
      const std::string_view rest = item.rest;
      const size_t split = rest.find('|');
      const std::vector<u8> second = ParseHex(rest.substr(0, split));
      const std::vector<u8> third = ParseHex(rest.substr(split + 1));
      const u32 output = PutFilled(c, 16);
      if (std::string_view(item.name).starts_with("hmac")) {
        c.Kernel("XeCryptHmacMd5", {CopyToGuest(c, key), key.size(), CopyToGuest(c, first),
                                    first.size(), 0, 0, 0, 0, output, 16});
      } else {
        c.Kernel("XeCryptMd5",
                 {first.empty() ? 0 : CopyToGuest(c, first), first.size(),
                  second.empty() ? 0 : CopyToGuest(c, second), second.size(),
                  third.empty() ? 0 : CopyToGuest(c, third), third.size(), output, 16});
      }
      CHECK(ToHex(ReadBytes(c, output, 16)) == item.output);
    }
  });
}

REX_TEST_CASE("XeCryptHmacMd5Final matches the measured RFC 2202 results", "[kernel][crypt][md5]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    for (const MeasuredCase& item : kMeasuredCases) {
      if (!std::string_view(item.name).starts_with("hmac")) {
        continue;
      }
      INFO(item.name);
      const std::vector<u8> key = ParseHex(item.key);
      const std::vector<u8> data = ParseHex(item.first);
      const u32 state = PutFilled(c, 0x200);
      c.Kernel("XeCryptHmacMd5Init", {state, CopyToGuest(c, key), key.size()});
      for (const u8 byte : data) {
        c.Kernel("XeCryptHmacMd5Update", {state, CopyToGuest(c, std::span<const u8>(&byte, 1)), 1});
      }
      const u32 output = PutFilled(c, 16);
      c.Kernel("XeCryptHmacMd5Final", {state, output, 16});
      CHECK(ToHex(ReadBytes(c, output, 16)) == item.output);
    }
  });
}

REX_TEST_CASE("The MD5 states match the measured layouts", "[kernel][crypt][md5]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    const std::vector<u8> abc = {'a', 'b', 'c'};
    const std::vector<u8> hmac_key(16, 0x0B);
    const u32 md5 = PutFilled(c, 0x200);
    c.Kernel("XeCryptMd5Init", {md5});
    CHECK(ToHex(ReadBytes(c, md5, 0x180)) == kMd5Init);
    c.Kernel("XeCryptMd5Update", {md5, CopyToGuest(c, abc), 3});
    CHECK(ToHex(ReadBytes(c, md5, 0x180)) == kMd5Update);

    const u32 hmac = PutFilled(c, 0x200);
    c.Kernel("XeCryptHmacMd5Init", {hmac, CopyToGuest(c, hmac_key), 16});
    CHECK(ToHex(ReadBytes(c, hmac, 0x180)) == kHmacInit);
    c.Kernel("XeCryptHmacMd5Update", {hmac, CopyToGuest(c, abc), 3});
    CHECK(ToHex(ReadBytes(c, hmac, 0x180)) == kHmacUpdate);
  });
}
