/**
 * @file        core/filesystem/svod.cpp
 * @brief       SVOD package parser
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_container_device.cc (Ben Vanik, 2020).
 */

#include "container_parse.h"

#include <cstring>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/math.h>

namespace rex::filesystem::detail {

namespace {

constexpr u64 kSVODBlockSize = 0x800;
constexpr u64 kHashBlockSize = 0x1000;
constexpr u64 kBlocksPerL0Hash = 0x198;
constexpr u64 kHashesPerL1Hash = 0xA1C4;
constexpr u64 kBlocksPerFile = 0x14388;
constexpr u64 kMaxFileSize = 0xA290000;
constexpr u64 kFileBlockSize = 0x1000;
constexpr std::string_view kMediaMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::string_view kXSFMagic = "XSF";

enum class Layout : u8 {
  kUnknown,
  kEnhancedGDF,
  kXSF,
  kSingleFile,
};

#pragma pack(push, 1)
struct RootData {
  u32 block;
  u32 size;
  u32 creation_date;
  u32 creation_time;
};
static_assert(sizeof(RootData) == 0x10);

struct DirectoryEntry {
  u16 node_l;
  u16 node_r;
  u32 data_block;
  u32 length;
  u8 attributes;
  u8 name_length;
};
static_assert(sizeof(DirectoryEntry) == 0xE);
#pragma pack(pop)

struct Location {
  u32 file = 0;
  u64 offset = 0;
};

bool HasMagic(std::span<const u8> bytes, u64 offset, std::string_view magic) {
  return InBounds(bytes, offset, magic.size()) &&
         std::memcmp(bytes.data() + offset, magic.data(), magic.size()) == 0;
}

class SVODReader {
 public:
  SVODReader(const STFSHeader& header, std::span<const std::span<const u8>> files)
      : header_(header), files_(files) {}

  Result<u64> DetectLayout() {
    std::span<const u8> first = files_[0];
    if (header_.metadata.volume_descriptor.svod.features.bits.enhanced_gdf_layout) {
      if (!HasMagic(first, 0x2000, kMediaMagic)) {
        return Err<u64>(Corrupt("SVOD uses the EGDF layout and has no magic block at 0x2000"));
      }
      layout_ = Layout::kEnhancedGDF;
      base_offset_ = 0;
      return 0x2000;
    }
    if (HasMagic(first, 0x12000, kMediaMagic)) {
      base_offset_ = 0x10000;
      layout_ = HasMagic(first, 0x2000, kXSFMagic) ? Layout::kXSF : Layout::kUnknown;
      return 0x12000;
    }
    if (HasMagic(first, 0xD000, kMediaMagic)) {
      base_offset_ = 0xB000;
      if (header_.metadata.data_file_count == 1) {
        layout_ = Layout::kSingleFile;
      } else {
        layout_ = Layout::kUnknown;
        REXLOG_ERROR(filesystem, "SVOD holds its magic block at 0xD000 and is not a single file");
      }
      return 0xD000;
    }
    return Err<u64>(Corrupt("SVOD magic block not found"));
  }

  [[nodiscard]] std::optional<Location> BlockToOffset(u64 block) const {
    const u64 block_offset = header_.metadata.volume_descriptor.svod.start_data_block();
    if (block < block_offset * 2) {
      return std::nullopt;
    }
    u64 true_block = block - (block_offset * 2);
    if (layout_ == Layout::kEnhancedGDF) {
      true_block += 0x2;
    }
    const u64 file_block = true_block % kBlocksPerFile;
    u64 file_index = true_block / kBlocksPerFile;
    const u64 level0_tables = (file_block / kBlocksPerL0Hash) + 1;
    const u64 level1_tables = (level0_tables / kHashesPerL1Hash) + 1;
    u64 offset = (level0_tables + level1_tables) * kHashBlockSize;
    if (layout_ == Layout::kSingleFile) {
      offset += base_offset_;
    }
    u64 address = (file_block * kSVODBlockSize) + offset;
    if (address >= kMaxFileSize) {
      file_index += 1;
      address %= kMaxFileSize;
      address += 0x2000;
    }
    if (file_index >= files_.size()) {
      return std::nullopt;
    }
    return Location{.file = static_cast<u32>(file_index), .offset = address};
  }

  Result<void> Walk(u32 root_block, std::vector<ContainerNode>& nodes) {
    std::vector<Visit> stack;
    stack.push_back({.block = root_block});
    while (!stack.empty()) {
      Visit visit = std::move(stack.back());
      stack.pop_back();
      if (visit.emit) {
        REX_TRY_VOID(Emit(std::move(visit), nodes, stack));
        continue;
      }
      REX_TRY_VOID(Expand(visit, stack));
    }
    return Ok();
  }

 private:
  struct Visit {
    u32 block = 0;
    u32 ordinal = 0;
    u32 parent = 0;
    bool emit = false;
    DirectoryEntry entry{};
    std::string name;
  };

  Result<void> Expand(const Visit& visit, std::vector<Visit>& stack) {
    if (!visited_.insert({visit.block, visit.ordinal}).second) {
      return Err(
          Corrupt(std::format("SVOD entry {} of block {} repeats", visit.ordinal, visit.block)));
    }
    const u64 ordinal_offset = static_cast<u64>(visit.ordinal) * 0x4;
    auto location = BlockToOffset(visit.block + (ordinal_offset / kSVODBlockSize));
    if (!location) {
      return Err(Corrupt(std::format("SVOD block {} is outside the data files", visit.block)));
    }
    const std::span<const u8> file = files_[location->file];
    const u64 address = location->offset + (ordinal_offset % kSVODBlockSize);
    auto entry = ReadAt<DirectoryEntry>(file, address);
    if (!entry || !InBounds(file, address + sizeof(DirectoryEntry), entry->name_length)) {
      return Err(
          Corrupt(std::format("SVOD directory entry at {:#x} is outside the data file", address)));
    }
    if (entry->node_r) {
      stack.push_back({.block = visit.block, .ordinal = entry->node_r, .parent = visit.parent});
    }
    stack.push_back({.block = visit.block,
                     .ordinal = visit.ordinal,
                     .parent = visit.parent,
                     .emit = true,
                     .entry = *entry,
                     .name = std::string(reinterpret_cast<const char*>(file.data() + address +
                                                                       sizeof(DirectoryEntry)),
                                         entry->name_length)});
    if (entry->node_l) {
      stack.push_back({.block = visit.block, .ordinal = entry->node_l, .parent = visit.parent});
    }
    return Ok();
  }

  Result<void> Emit(Visit visit, std::vector<ContainerNode>& nodes,
                    std::vector<Visit>& stack) const {
    const DirectoryEntry& entry = visit.entry;
    const u64 root_time = nodes.front().create_time;
    ContainerNode node;
    node.name = std::move(visit.name);
    node.parent = visit.parent;
    node.create_time = root_time;
    node.access_time = root_time;
    node.write_time = root_time;
    const bool directory = (entry.attributes & std::to_underlying(FileAttributes::kDirectory)) != 0;
    if (directory) {
      node.attributes = FileAttributes::kDirectory | FileAttributes::kReadOnly;
    } else {
      node.attributes = FileAttributes::kNormal | FileAttributes::kReadOnly;
      node.size = entry.length;
      node.allocation_size = round_up(static_cast<u64>(entry.length), kFileBlockSize);
      u64 data_block = entry.data_block;
      u64 remaining = round_up(static_cast<u64>(entry.length), kSVODBlockSize);
      while (remaining) {
        auto data = BlockToOffset(data_block);
        if (!data) {
          return Err(Corrupt(std::format("SVOD data block {} of '{}' is outside the data files",
                                         data_block, node.name)));
        }
        ++data_block;
        remaining -= kSVODBlockSize;
        if (!node.extents.empty()) {
          ContainerExtent& last = node.extents.back();
          if (last.file == data->file && last.offset + last.length == data->offset) {
            last.length += kSVODBlockSize;
            continue;
          }
        }
        node.extents.push_back(
            {.file = data->file, .offset = data->offset, .length = kSVODBlockSize});
      }
    }
    const auto index = static_cast<u32>(nodes.size());
    nodes.push_back(std::move(node));
    nodes[visit.parent].children.push_back(index);
    if (directory && entry.length) {
      stack.push_back({.block = entry.data_block, .parent = index});
    }
    return Ok();
  }

  const STFSHeader& header_;
  std::span<const std::span<const u8>> files_;
  Layout layout_ = Layout::kUnknown;
  u64 base_offset_ = 0;
  std::set<std::pair<u32, u32>> visited_;
};

}  // namespace

Result<void> ParseSVOD(const STFSHeader& header, std::span<const std::span<const u8>> files,
                       std::vector<ContainerNode>& nodes) {
  if (files.empty()) {
    return Err(Corrupt("SVOD has no data file"));
  }
  SVODReader reader(header, files);
  REX_TRY(const u64 magic_offset, reader.DetectLayout());
  auto root = ReadAt<RootData>(files[0], magic_offset + 0x14);
  if (!root) {
    return Err(Corrupt("SVOD root record is outside the data file"));
  }
  const u64 root_time = decode_fat_timestamp(root->creation_date, root->creation_time);
  nodes.front().attributes = FileAttributes::kDirectory;
  nodes.front().create_time = root_time;
  nodes.front().access_time = root_time;
  nodes.front().write_time = root_time;
  return reader.Walk(root->block, nodes);
}

}  // namespace rex::filesystem::detail
