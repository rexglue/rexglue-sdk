/**
 * @file        tests/unit/base/xex_builder.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/types.h>

#include "excrypt.h"

namespace rex::test {

using Key = std::array<u8, 16>;

inline constexpr std::array<Key, 4> kMasterKeys = {{
    {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF,
     0x91},
    {},
    {0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9, 0x8B, 0x99, 0x92, 0x2C, 0xE9, 0x32, 0x15,
     0x72},
    {0xA8, 0xB0, 0x05, 0x12, 0xED, 0xE3, 0x63, 0x8D, 0xC6, 0x58, 0xB3, 0x10, 0x1F, 0x9F, 0x50,
     0xD1},
}};
inline constexpr Key kSessionKey = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

struct XEXImportSpec {
  std::string name;
  std::vector<u32> records;
  u32 id = 0;
};

struct XEXSpec {
  kernel::XEXCompressionType compression = kernel::kXEXCompressionNone;
  bool encrypted = false;
  filesystem::XEXKey key = filesystem::XEXKey::kRetail;
  bool page_size_4kb = true;
  u32 module_flags = kernel::kXEXModuleTitle;
  u32 load_address = 0x82000000;
  u32 entry_point = 0x82000100;
  u32 export_table = 0;
  u32 title_id = 0x4D5307E6;
  std::vector<u32> alternate_title_ids;
  std::vector<u8> image;
  std::vector<u8> patch_descriptor;
  std::vector<u8> patch_body;
  std::vector<XEXImportSpec> imports;
};

inline void PutBE32(std::vector<u8>& out, size_t offset, u32 value) {
  out[offset] = static_cast<u8>(value >> 24);
  out[offset + 1] = static_cast<u8>(value >> 16);
  out[offset + 2] = static_cast<u8>(value >> 8);
  out[offset + 3] = static_cast<u8>(value);
}

inline void PutBE16(std::vector<u8>& out, size_t offset, u16 value) {
  out[offset] = static_cast<u8>(value >> 8);
  out[offset + 1] = static_cast<u8>(value);
}

inline void AppendBE32(std::vector<u8>& out, u32 value) {
  out.resize(out.size() + 4);
  PutBE32(out, out.size() - 4, value);
}

inline std::array<u8, 20> SHA1Of(std::span<const u8> data) {
  std::array<u8, 20> digest{};
  ExCryptSha(data.data(), static_cast<u32>(data.size()), nullptr, 0, nullptr, 0, digest.data(),
             static_cast<u32>(digest.size()));
  return digest;
}

inline std::vector<u8> EncryptCBC(const Key& key, std::span<const u8> input) {
  alignas(16) EXCRYPT_AES_STATE state{};
  ExCryptAesKey(&state, key.data());
  std::vector<u8> out(input.size());
  std::array<u8, 16> chain{};
  ExCryptAesCbc(&state, input.data(), static_cast<u32>(input.size() & ~size_t{15}), out.data(),
                chain.data(), 1);
  return out;
}

inline Key EncryptKey(const Key& key, const Key& value) {
  const auto out = EncryptCBC(key, value);
  Key result{};
  std::memcpy(result.data(), out.data(), 16);
  return result;
}

inline std::vector<u8> LZXStoredStream(std::span<const u8> data) {
  std::vector<u8> out;
  const u32 bits = (3u << 28) | (static_cast<u32>(data.size()) << 4);
  for (const u16 word : {static_cast<u16>(bits >> 16), static_cast<u16>(bits & 0xFFFF)}) {
    out.push_back(static_cast<u8>(word & 0xFF));
    out.push_back(static_cast<u8>(word >> 8));
  }
  for (int r = 0; r < 3; ++r) {
    out.insert(out.end(), {1, 0, 0, 0});
  }
  out.insert(out.end(), data.begin(), data.end());
  if (data.size() & 1) {
    out.push_back(0);
  }
  return out;
}

inline std::vector<u8> CompressedBlock(std::span<const u8> payload, size_t chunk_limit = 0xFFFF) {
  std::vector<u8> block(24, 0);
  for (size_t offset = 0; offset < payload.size(); offset += chunk_limit) {
    const size_t chunk = std::min(chunk_limit, payload.size() - offset);
    block.push_back(static_cast<u8>(chunk >> 8));
    block.push_back(static_cast<u8>(chunk));
    const auto piece = payload.subspan(offset, chunk);
    block.insert(block.end(), piece.begin(), piece.end());
  }
  block.push_back(0);
  block.push_back(0);
  return block;
}

inline std::vector<u8> BuildXEX(const XEXSpec& spec) {
  const bool patch = !spec.patch_descriptor.empty();
  std::vector<u8> format;
  std::vector<u8> data;
  const Key& master = kMasterKeys[static_cast<size_t>(spec.key)];
  auto encrypt = [&](std::span<const u8> plain) {
    return spec.encrypted ? EncryptCBC(kSessionKey, plain)
                          : std::vector<u8>(plain.begin(), plain.end());
  };
  auto normal_format = [&](std::vector<u8> block) {
    format.assign(36, 0);
    PutBE32(format, 0, 36);
    PutBE16(format, 4, spec.encrypted ? kernel::kXEXEncryptionNormal : kernel::kXEXEncryptionNone);
    PutBE16(format, 6, static_cast<u16>(spec.compression));
    PutBE32(format, 8, 0x8000);
    PutBE32(format, 12, static_cast<u32>(block.size()));
    const auto data_bytes = encrypt(block);
    const auto hash = SHA1Of(block);
    std::memcpy(format.data() + 16, hash.data(), 20);
    data = data_bytes;
  };
  if (patch) {
    normal_format(spec.patch_body);
  } else if (spec.compression == kernel::kXEXCompressionNone) {
    format.assign(8, 0);
    PutBE32(format, 0, 8);
    PutBE16(format, 4, spec.encrypted ? kernel::kXEXEncryptionNormal : kernel::kXEXEncryptionNone);
    PutBE16(format, 6, kernel::kXEXCompressionNone);
    data = encrypt(spec.image);
  } else if (spec.compression == kernel::kXEXCompressionBasic) {
    const u32 half = static_cast<u32>(spec.image.size() / 2);
    format.assign(16, 0);
    PutBE32(format, 0, 16);
    PutBE16(format, 4, spec.encrypted ? kernel::kXEXEncryptionNormal : kernel::kXEXEncryptionNone);
    PutBE16(format, 6, kernel::kXEXCompressionBasic);
    PutBE32(format, 8, half);
    PutBE32(format, 12, static_cast<u32>(spec.image.size()) - half);
    data = encrypt(std::span(spec.image).first(half));
  } else {
    normal_format(CompressedBlock(LZXStoredStream(spec.image)));
  }

  std::vector<u8> alt_ids;
  if (!spec.alternate_title_ids.empty()) {
    AppendBE32(alt_ids, 4 + 4 * static_cast<u32>(spec.alternate_title_ids.size()));
    for (const u32 id : spec.alternate_title_ids) {
      AppendBE32(alt_ids, id);
    }
  }
  std::vector<u8> exec(0x18, 0);
  PutBE32(exec, 0xC, spec.title_id);

  std::vector<u8> import_libraries;
  if (!spec.imports.empty()) {
    std::vector<u8> names;
    for (const XEXImportSpec& library : spec.imports) {
      names.insert(names.end(), library.name.begin(), library.name.end());
      names.push_back(0);
      while (names.size() % 4) {
        names.push_back(0);
      }
    }
    AppendBE32(import_libraries, 0);
    AppendBE32(import_libraries, static_cast<u32>(names.size()));
    AppendBE32(import_libraries, static_cast<u32>(spec.imports.size()));
    import_libraries.insert(import_libraries.end(), names.begin(), names.end());
    for (size_t index = 0; index < spec.imports.size(); ++index) {
      const XEXImportSpec& library = spec.imports[index];
      const size_t start = import_libraries.size();
      import_libraries.resize(start + 0x28, 0);
      PutBE32(import_libraries, start, 0x28 + 4 * static_cast<u32>(library.records.size()));
      PutBE32(import_libraries, start + 0x18, library.id);
      PutBE16(import_libraries, start + 0x24, static_cast<u16>(index));
      PutBE16(import_libraries, start + 0x26, static_cast<u16>(library.records.size()));
      for (const u32 record : library.records) {
        AppendBE32(import_libraries, record);
      }
    }
    PutBE32(import_libraries, 0, static_cast<u32>(import_libraries.size()));
  }

  struct Entry {
    u32 key;
    std::vector<u8> blob;
    u32 value;
  };
  std::vector<Entry> entries = {
      {kernel::kXEXHeaderFileFormatInfo, format, 0},
      {kernel::kXEXHeaderEntryPoint, {}, spec.entry_point},
      {kernel::kXEXHeaderExecutionInfo, exec, 0},
  };
  if (!alt_ids.empty()) {
    entries.push_back({kernel::kXEXHeaderAlternateTitleIDs, alt_ids, 0});
  }
  if (patch) {
    entries.push_back({kernel::kXEXHeaderDeltaPatchDescriptor, spec.patch_descriptor, 0});
  }
  if (!import_libraries.empty()) {
    entries.push_back({kernel::kXEXHeaderImportLibraries, import_libraries, 0});
  }

  std::vector<u8> header(0x18 + entries.size() * 8, 0);
  PutBE32(header, 0x0, 0x58455832);
  PutBE32(header, 0x4, spec.module_flags);
  PutBE32(header, 0x14, static_cast<u32>(entries.size()));
  for (size_t i = 0; i < entries.size(); ++i) {
    PutBE32(header, 0x18 + i * 8, entries[i].key);
    if (entries[i].blob.empty()) {
      PutBE32(header, 0x18 + i * 8 + 4, entries[i].value);
    } else {
      while (header.size() % 4) {
        header.push_back(0);
      }
      PutBE32(header, 0x18 + i * 8 + 4, static_cast<u32>(header.size()));
      header.insert(header.end(), entries[i].blob.begin(), entries[i].blob.end());
    }
  }
  while (header.size() % 4) {
    header.push_back(0);
  }

  const u32 page_size = spec.page_size_4kb ? 0x1000 : 0x10000;
  const u32 pages =
      std::max<u32>(1, static_cast<u32>((spec.image.size() + page_size - 1) / page_size));
  const size_t security_offset = header.size();
  PutBE32(header, 0x10, static_cast<u32>(security_offset));
  header.resize(security_offset + 0x184 + 0x18, 0);
  PutBE32(header, security_offset + 0x4, pages * page_size);
  PutBE32(header, security_offset + 0x10C, spec.page_size_4kb ? kernel::kXEXImagePageSize4KB : 0);
  PutBE32(header, security_offset + 0x110, spec.load_address);
  const Key encrypted_key = EncryptKey(master, kSessionKey);
  std::memcpy(header.data() + security_offset + 0x150, encrypted_key.data(), 16);
  PutBE32(header, security_offset + 0x160, spec.export_table);
  PutBE32(header, security_offset + 0x180, 1);
  PutBE32(header, security_offset + 0x184, (pages << 4) | kernel::kXEXSectionCode);
  PutBE32(header, 0x8, static_cast<u32>(header.size()));

  header.insert(header.end(), data.begin(), data.end());
  return header;
}

inline std::vector<u8> TestImage(size_t size) {
  std::vector<u8> image(size, 0);
  image[0] = 0x4D;
  image[1] = 0x5A;
  image[2] = 0x90;
  for (size_t i = 16; i < size / 2; ++i) {
    image[i] = static_cast<u8>(i * 7 + 3);
  }
  return image;
}

inline std::vector<u8> BuildPatchBody(u32 copy_to = 0x800, u32 zero_at = 0x10) {
  std::vector<u8> records;
  auto record = [&](u32 old_addr, u32 new_addr, u16 length, u16 compressed) {
    const size_t at = records.size();
    records.resize(at + 12, 0);
    PutBE32(records, at, old_addr);
    PutBE32(records, at + 4, new_addr);
    PutBE16(records, at + 8, length);
    PutBE16(records, at + 10, compressed);
  };
  record(0x0, copy_to, 0x10, 1);
  record(0x0, zero_at, 0x10, 0);
  record(0, 0, 0, 0);
  std::vector<u8> block(24, 0);
  block.insert(block.end(), records.begin(), records.end());
  return block;
}

inline std::vector<u8> BuildPatchDescriptor(const filesystem::IXEXImage& base) {
  std::vector<u8> desc(0x4C + 13, 0);
  PutBE32(desc, 0x0, static_cast<u32>(desc.size()));
  const auto digest = SHA1Of(base.GetSecurity().rsa_signature);
  std::memcpy(desc.data() + 0xC, digest.data(), 20);
  const auto key_source = EncryptKey(kSessionKey, kSessionKey);
  std::memcpy(desc.data() + 0x20, key_source.data(), 16);
  PutBE32(desc, 0x30, static_cast<u32>(base.GetHeaderBytes().size()));
  PutBE32(desc, 0x44, base.GetImageSize());
  PutBE32(desc, 0x4C, 0);
  PutBE32(desc, 0x50, 0);
  PutBE16(desc, 0x54, 4);
  PutBE16(desc, 0x56, 1);
  return desc;
}

}  // namespace rex::test
