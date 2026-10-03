/**
 * @file        core/filesystem/gdfx.cpp
 * @brief       GDFX disc image parser
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/disc_image_device.cc (Ben Vanik, 2020).
 */

#include "container_parse.h"

#include <array>
#include <cstring>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/literals.h>
#include <rex/math.h>

namespace rex::filesystem::detail {

namespace {

constexpr u64 kSectorSize = 2_KiB;
constexpr u64 kBytesPerSector = 0x200;
constexpr u64 kMagicSector = 32;
constexpr u64 kMinRootSize = 13;
constexpr u64 kMaxRootSize = 32_MiB;
constexpr u64 kEntryHeaderSize = 14;
constexpr std::array<u64, 5> kGameOffsets = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000,
                                             0x0FD90000};
constexpr std::string_view kMagic = "MICROSOFT*XBOX*MEDIA";

constexpr u64 kUnixEpochTicks = 10000 * 11644473600000LL;

bool MagicAt(std::span<const u8> image, u64 offset) {
  return InBounds(image, offset, kMagic.size()) &&
         std::memcmp(image.data() + offset, kMagic.data(), kMagic.size()) == 0;
}

std::optional<u64> FindGameOffset(std::span<const u8> image) {
  for (const u64 offset : kGameOffsets) {
    if (MagicAt(image, offset + (kMagicSector * kSectorSize))) {
      return offset;
    }
  }
  return std::nullopt;
}

struct Visit {
  u64 table = 0;
  u32 ordinal = 0;
  u32 parent = 0;
  bool emit = false;
  u32 sector = 0;
  u32 length = 0;
  u8 attributes = 0;
  std::string name;
};

class GDFXReader {
 public:
  GDFXReader(std::span<const u8> image, u64 game_offset)
      : image_(image), game_offset_(game_offset) {}

  Result<void> Walk(u64 root_table, std::vector<ContainerNode>& nodes) {
    std::vector<Visit> stack;
    stack.push_back({.table = root_table});
    while (!stack.empty()) {
      Visit visit = std::move(stack.back());
      stack.pop_back();
      if (visit.emit) {
        Emit(std::move(visit), nodes, stack);
        continue;
      }
      REX_TRY_VOID(Expand(visit, stack));
    }
    return Ok();
  }

 private:
  Result<void> Expand(const Visit& visit, std::vector<Visit>& stack) {
    if (!visited_.insert({visit.table, visit.ordinal}).second) {
      return Err(Corrupt(
          std::format("GDFX entry {} of the table at {:#x} repeats", visit.ordinal, visit.table)));
    }
    const u64 entry_offset = static_cast<u64>(visit.ordinal) * 4;
    if (!InBounds(image_, visit.table + entry_offset, kEntryHeaderSize)) {
      return Err(Corrupt(std::format("GDFX entry {} of the table at {:#x} is outside the image",
                                     visit.ordinal, visit.table)));
    }
    const u8* p = image_.data() + visit.table + entry_offset;
    u16 node_l = 0;
    u16 node_r = 0;
    Visit entry{
        .table = visit.table, .ordinal = visit.ordinal, .parent = visit.parent, .emit = true};
    std::memcpy(&node_l, p + 0, sizeof(node_l));
    std::memcpy(&node_r, p + 2, sizeof(node_r));
    std::memcpy(&entry.sector, p + 4, sizeof(entry.sector));
    std::memcpy(&entry.length, p + 8, sizeof(entry.length));
    entry.attributes = p[12];
    const u8 name_length = p[13];
    if (!InBounds(image_, visit.table + entry_offset + kEntryHeaderSize, name_length)) {
      return Err(Corrupt(std::format("GDFX name of entry {} is outside the image", visit.ordinal)));
    }
    entry.name.assign(reinterpret_cast<const char*>(p + kEntryHeaderSize), name_length);
    if (node_r) {
      stack.push_back({.table = visit.table, .ordinal = node_r, .parent = visit.parent});
    }
    stack.push_back(std::move(entry));
    if (node_l) {
      stack.push_back({.table = visit.table, .ordinal = node_l, .parent = visit.parent});
    }
    return Ok();
  }

  void Emit(Visit visit, std::vector<ContainerNode>& nodes, std::vector<Visit>& stack) {
    ContainerNode node;
    node.name = std::move(visit.name);
    node.parent = visit.parent;
    node.attributes = static_cast<FileAttributes>(visit.attributes) | FileAttributes::kReadOnly;
    node.size = visit.length;
    node.allocation_size = round_up(static_cast<u64>(visit.length), kBytesPerSector);
    node.create_time = kUnixEpochTicks;
    node.access_time = kUnixEpochTicks;
    node.write_time = kUnixEpochTicks;
    const u64 data_offset = game_offset_ + (static_cast<u64>(visit.sector) * kSectorSize);
    const bool directory = HasFlags(node.attributes, FileAttributes::kDirectory);
    if (!directory) {
      node.extents.push_back({.file = 0, .offset = data_offset, .length = visit.length});
    }
    const auto index = static_cast<u32>(nodes.size());
    nodes.push_back(std::move(node));
    nodes[visit.parent].children.push_back(index);
    if (directory && visit.length) {
      stack.push_back({.table = data_offset, .parent = index});
    }
  }

  std::span<const u8> image_;
  u64 game_offset_ = 0;
  std::set<std::pair<u64, u32>> visited_;
};

}  // namespace

bool IsGDFX(std::span<const u8> image) {
  return FindGameOffset(image).has_value();
}

Result<void> ParseGDFX(std::span<const u8> image, std::vector<ContainerNode>& nodes) {
  auto game_offset = FindGameOffset(image);
  if (!game_offset) {
    return Err(Corrupt("GDFX magic not found"));
  }
  const u64 volume = *game_offset + (kMagicSector * kSectorSize);
  auto root_sector = ReadAt<u32>(image, volume + 20);
  auto root_size = ReadAt<u32>(image, volume + 24);
  if (!root_sector || !root_size) {
    return Err(Corrupt("GDFX volume descriptor is outside the image"));
  }
  if (*root_size < kMinRootSize || *root_size > kMaxRootSize) {
    return Err(Corrupt(std::format("GDFX root table size {} is out of range", *root_size)));
  }
  const u64 root_offset = *game_offset + (static_cast<u64>(*root_sector) * kSectorSize);
  if (!InBounds(image, root_offset, *root_size)) {
    return Err(Corrupt("GDFX root table is outside the image"));
  }
  nodes.front().attributes = FileAttributes::kDirectory;
  GDFXReader reader(image, *game_offset);
  return reader.Walk(root_offset, nodes);
}

}  // namespace rex::filesystem::detail
