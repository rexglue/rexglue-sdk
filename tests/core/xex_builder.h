/**
 * @file        tests/core/xex_builder.h
 * @brief       Builds synthetic XEX files for the parser tests
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
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/types.h>

#include "crypto/TinySHA1.hpp"
#include "crypto/rijndael-alg-fst.h"

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

struct XEXSpec {
  xex2_compression_type compression = XEX_COMPRESSION_NONE;
  bool encrypted = false;
  filesystem::XEXKey key = filesystem::XEXKey::Retail;
  bool page_size_4kb = true;
  u32 module_flags = XEX_MODULE_TITLE;
  u32 load_address = 0x82000000;
  u32 entry_point = 0x82000100;
  u32 title_id = 0x4D5307E6;
  std::vector<u32> alternate_title_ids;
  std::vector<u8> image;
  std::vector<u8> patch_descriptor;
  std::vector<u8> patch_body;
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
  sha1::SHA1 sha;
  sha.processBytes(data.data(), data.size());
  std::array<u8, 20> digest{};
  sha.finalize(digest.data());
  return digest;
}

inline std::vector<u8> EncryptCBC(const Key& key, std::span<const u8> input) {
  u32 round_keys[4 * (MAXNR + 1)];
  const int rounds = rijndaelKeySetupEnc(round_keys, key.data(), 128);
  std::vector<u8> out(input.size());
  std::array<u8, 16> chain{};
  for (size_t offset = 0; offset + 16 <= input.size(); offset += 16) {
    std::array<u8, 16> block{};
    for (size_t i = 0; i < 16; ++i) {
      block[i] = input[offset + i] ^ chain[i];
    }
    rijndaelEncrypt(round_keys, rounds, block.data(), out.data() + offset);
    std::memcpy(chain.data(), out.data() + offset, 16);
  }
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
    PutBE16(format, 4, spec.encrypted ? XEX_ENCRYPTION_NORMAL : XEX_ENCRYPTION_NONE);
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
  } else if (spec.compression == XEX_COMPRESSION_NONE) {
    format.assign(8, 0);
    PutBE32(format, 0, 8);
    PutBE16(format, 4, spec.encrypted ? XEX_ENCRYPTION_NORMAL : XEX_ENCRYPTION_NONE);
    PutBE16(format, 6, XEX_COMPRESSION_NONE);
    data = encrypt(spec.image);
  } else if (spec.compression == XEX_COMPRESSION_BASIC) {
    const u32 half = static_cast<u32>(spec.image.size() / 2);
    format.assign(16, 0);
    PutBE32(format, 0, 16);
    PutBE16(format, 4, spec.encrypted ? XEX_ENCRYPTION_NORMAL : XEX_ENCRYPTION_NONE);
    PutBE16(format, 6, XEX_COMPRESSION_BASIC);
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

  struct Entry {
    u32 key;
    std::vector<u8> blob;
    u32 value;
  };
  std::vector<Entry> entries = {
      {XEX_HEADER_FILE_FORMAT_INFO, format, 0},
      {XEX_HEADER_ENTRY_POINT, {}, spec.entry_point},
      {XEX_HEADER_EXECUTION_INFO, exec, 0},
  };
  if (!alt_ids.empty()) {
    entries.push_back({XEX_HEADER_ALTERNATE_TITLE_IDS, alt_ids, 0});
  }
  if (patch) {
    entries.push_back({XEX_HEADER_DELTA_PATCH_DESCRIPTOR, spec.patch_descriptor, 0});
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
  PutBE32(header, security_offset + 0x10C, spec.page_size_4kb ? XEX_IMAGE_PAGE_SIZE_4KB : 0);
  PutBE32(header, security_offset + 0x110, spec.load_address);
  const Key encrypted_key = EncryptKey(master, kSessionKey);
  std::memcpy(header.data() + security_offset + 0x150, encrypted_key.data(), 16);
  PutBE32(header, security_offset + 0x180, 1);
  PutBE32(header, security_offset + 0x184, (pages << 4) | XEX_SECTION_CODE);
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

}  // namespace rex::test
