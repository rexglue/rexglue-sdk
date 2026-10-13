/**
 * @file        tests/unit/core/xex_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstring>
#include <vector>

#include <rex/filesystem/xex.h>

#include "pe/pe_image.h"
#include "xex_builder.h"

namespace rex {

namespace {

using filesystem::XEXImage;
using test::BuildXEX;
using test::TestImage;
using test::XEXSpec;

std::vector<u8> DecodeAll(std::span<const u8> file, XEXImage& image) {
  std::vector<u8> out(image.GetImageSize(), 0xCC);
  REQUIRE(image.Decode(file, out));
  return out;
}

u32 GetBE32(std::span<const u8> data, size_t offset) {
  return (u32{data[offset]} << 24) | (u32{data[offset + 1]} << 16) | (u32{data[offset + 2]} << 8) |
         u32{data[offset + 3]};
}

void MoveOptHeader(std::vector<u8>& file, u32 key, u32 value) {
  const u32 count = GetBE32(file, 0x14);
  for (u32 i = 0; i < count; ++i) {
    const size_t entry = 0x18 + i * 8;
    if (GetBE32(file, entry) == key) {
      test::PutBE32(file, entry + 4, value);
      return;
    }
  }
  FAIL("optional header key not present");
}

}  // namespace

REX_TEST_CASE("XEX optional headers that end past the header are not returned", "[core][xex]") {
  XEXSpec spec;
  spec.image = TestImage(0x1000);
  spec.alternate_title_ids = {0x11111111, 0x22222222};
  auto file = BuildXEX(spec);
  const u32 header_size = GetBE32(file, 0x8);

  auto fixed = file;
  MoveOptHeader(fixed, filesystem::kXEXHeaderExecutionInfo, header_size - 4);
  auto fixed_image = XEXImage::Parse(fixed);
  REQUIRE(fixed_image);
  CHECK_FALSE(
      fixed_image->OptHeader<filesystem::XEXOptExecutionInfo>(filesystem::kXEXHeaderExecutionInfo));

  auto variable = file;
  MoveOptHeader(variable, filesystem::kXEXHeaderAlternateTitleIds, header_size - 2);
  auto variable_image = XEXImage::Parse(variable);
  REQUIRE(variable_image);
  CHECK_FALSE(variable_image->OptHeader<filesystem::XEXOptGenericU32>(
      filesystem::kXEXHeaderAlternateTitleIds));

  auto long_length = file;
  MoveOptHeader(long_length, filesystem::kXEXHeaderAlternateTitleIds, header_size - 8);
  test::PutBE32(long_length, header_size - 8, 0x100);
  auto long_image = XEXImage::Parse(long_length);
  REQUIRE(long_image);
  CHECK_FALSE(
      long_image->OptHeader<filesystem::XEXOptGenericU32>(filesystem::kXEXHeaderAlternateTitleIds));
  CHECK(long_image->GetAlternateTitleIds().empty());
}

REX_TEST_CASE("XEX header and optional headers parse", "[core][xex]") {
  XEXSpec spec;
  spec.image = TestImage(0x1000);
  spec.alternate_title_ids = {0x11111111, 0x22222222};
  const auto file = BuildXEX(spec);
  auto image = XEXImage::Parse(file);
  REQUIRE(image);
  CHECK(image->OptHeaderValue(filesystem::kXEXHeaderEntryPoint) == 0x82000100u);
  const auto* exec =
      image->OptHeader<filesystem::XEXOptExecutionInfo>(filesystem::kXEXHeaderExecutionInfo);
  REQUIRE(exec);
  CHECK(static_cast<u32>(exec->title_id) == 0x4D5307E6u);
  CHECK(image->GetAlternateTitleIds() == std::vector<u32>{0x11111111, 0x22222222});
  CHECK_FALSE(image->OptHeaderValue(filesystem::kXEXHeaderDefaultStackSize));
  CHECK(image->GetSecurity().load_address == 0x82000000u);
  REQUIRE(image->GetSecurity().pages.size() == 1);
  CHECK(image->GetSecurity().pages[0].type == filesystem::kXEXSectionCode);
  CHECK(image->GetSecurity().pages[0].page_count == 1u);
  CHECK_FALSE(image->IsPatch());
}

REX_TEST_CASE("XEX image size follows the 4 KB page flag", "[core][xex]") {
  XEXSpec spec;
  spec.image = TestImage(0x1000);
  auto small = XEXImage::Parse(BuildXEX(spec));
  REQUIRE(small);
  CHECK(small->GetImageSize() == 0x1000u);
  spec.page_size_4kb = false;
  auto large = XEXImage::Parse(BuildXEX(spec));
  REQUIRE(large);
  CHECK(large->GetImageSize() == 0x10000u);
}

REX_TEST_CASE("XEX decodes every compression and key", "[core][xex]") {
  const auto expected = TestImage(0x1000);
  for (const auto compression : {filesystem::kXEXCompressionNone, filesystem::kXEXCompressionBasic,
                                 filesystem::kXEXCompressionNormal}) {
    for (const bool encrypted : {false, true}) {
      for (const auto key : {filesystem::XEXKey::kRetail, filesystem::XEXKey::kDevKit,
                             filesystem::XEXKey::kRetailXEX1, filesystem::XEXKey::kDevKitXEX1}) {
        XEXSpec spec;
        spec.compression = compression;
        spec.encrypted = encrypted;
        spec.key = key;
        spec.image = expected;
        const auto file = BuildXEX(spec);
        auto image = XEXImage::Parse(file);
        REQUIRE(image);
        const auto decoded = DecodeAll(file, *image);
        CHECK(decoded == expected);
        if (encrypted) {
          CHECK(image->GetKey() == key);
        }
      }
    }
  }
}

REX_TEST_CASE("XEX rejects bad input", "[core][xex]") {
  XEXSpec spec;
  spec.image = TestImage(0x1000);
  auto file = BuildXEX(spec);

  auto bad_magic = file;
  bad_magic[3] = 'Z';
  CHECK_FALSE(XEXImage::Parse(bad_magic));

  CHECK_FALSE(XEXImage::Parse(std::span(file).first(0x20)));

  spec.compression = filesystem::kXEXCompressionNormal;
  auto compressed = BuildXEX(spec);
  compressed[compressed.size() - 100] ^= 0xFF;
  auto image = XEXImage::Parse(compressed);
  REQUIRE(image);
  std::vector<u8> out(image->GetImageSize());
  const auto result = image->Decode(compressed, out);
  REQUIRE_FALSE(result);
  CHECK(result.error().category == ErrorCategory::kCrypto);
}

REX_TEST_CASE("XEX delta patch applies to header and image", "[core][xex]") {
  XEXSpec base_spec;
  base_spec.image = TestImage(0x1000);
  const auto base_file = BuildXEX(base_spec);
  auto base = XEXImage::Parse(base_file);
  REQUIRE(base);
  auto decoded = DecodeAll(base_file, *base);

  XEXSpec patch_spec;
  patch_spec.module_flags = filesystem::kXEXModulePatchDelta;
  patch_spec.compression = filesystem::kXEXCompressionDelta;
  patch_spec.patch_descriptor = test::BuildPatchDescriptor(*base);
  patch_spec.patch_body = test::BuildPatchBody();
  const auto patch_file = BuildXEX(patch_spec);
  auto patch = XEXImage::Parse(patch_file);
  REQUIRE(patch);
  REQUIRE(patch->IsPatch());

  auto patched = base->ApplyHeaderDelta(*patch);
  REQUIRE(patched);
  CHECK(patched->GetHeaderBytes().size() == base->GetHeaderBytes().size());
  REQUIRE(patched->ApplyImageDelta(*patch, decoded));
  CHECK(std::memcmp(decoded.data() + 0x800, decoded.data(), 0x10) == 0);
  for (size_t i = 0x10; i < 0x20; ++i) {
    CHECK(decoded[i] == 0);
  }
}

REX_TEST_CASE("PE headers parse from a decoded image", "[core][xex]") {
  std::vector<u8> image(0x2000, 0);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
  dos->e_magic = IMAGE_DOS_SIGNATURE;
  dos->e_cblp = 0x90;
  dos->e_lfanew = 0x80;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + 0x80);
  nt->Signature = IMAGE_NT_SIGNATURE;
  nt->FileHeader.Machine = IMAGE_FILE_MACHINE_POWERPCBE;
  nt->FileHeader.NumberOfSections = 1;
  nt->FileHeader.TimeDateStamp = 0x5F000000;
  nt->FileHeader.SizeOfOptionalHeader = IMAGE_SIZEOF_NT_OPTIONAL_HEADER;
  nt->FileHeader.Characteristics = IMAGE_FILE_32BIT_MACHINE;
  nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
  nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_XBOX;
  nt->OptionalHeader.SizeOfHeaders = 0x1000;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress = 0x1000;
  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = 0x40;
  auto* section = reinterpret_cast<IMAGE_SECTION_HEADER*>(image.data() + 0x80 + 24 +
                                                          IMAGE_SIZEOF_NT_OPTIONAL_HEADER);
  std::memcpy(section->Name, ".text\0\0\0", 8);
  section->VirtualAddress = 0x1000;
  section->Misc.VirtualSize = 0x200;
  section->Characteristics = filesystem::kPESectionMemoryExecute;

  auto info = filesystem::ParsePE(image);
  REQUIRE(info);
  CHECK(info->time_date_stamp == 0x5F000000u);
  CHECK(info->exception_directory_rva == 0x1000u);
  CHECK(info->exception_directory_size == 0x40u);
  REQUIRE(info->sections.size() == 1);
  CHECK(std::string(info->sections[0].name) == ".text");
  CHECK(info->sections[0].address == 0x1000u);
  CHECK(info->header_size == 0x1000u);

  nt->OptionalHeader.SizeOfHeaders = 0x4000;
  info = filesystem::ParsePE(image);
  REQUIRE(info);
  CHECK(info->header_size == 0x1000u);

  nt->OptionalHeader.SizeOfHeaders = 0;
  CHECK_FALSE(filesystem::ParsePE(image));
  nt->OptionalHeader.SizeOfHeaders = 0x80;
  CHECK_FALSE(filesystem::ParsePE(image));
  nt->OptionalHeader.SizeOfHeaders = 0x1000;

  nt->FileHeader.Machine = 0x14C;
  CHECK_FALSE(filesystem::ParsePE(image));
}

}  // namespace rex
