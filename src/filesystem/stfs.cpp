/**
 * @file        filesystem/stfs.cpp
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

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <unordered_map>

#include <rex/chrono.h>
#include <rex/logging/log.h>
#include <rex/filesystem/file_stat.h>
#include <rex/math.h>

namespace rex::filesystem::detail {

namespace {

constexpr u32 kEndOfChain = 0xFFFFFF;
constexpr u32 kEntriesPerDirectoryBlock = kSTFSBlockSize / sizeof(STFSDirectoryEntry);
constexpr u16 kRootDirectoryIndex = 0xFFFF;

class STFSReader {
 public:
  STFSReader(const XContentPackageHeader& header, std::span<const u8> file)
      : header_(header), file_(file) {
    const auto& descriptor = header_.metadata.volume_descriptor.stfs;
    blocks_per_hash_table_ = descriptor.flags.bits.read_only_format ? 1 : 2;
    block_step_[0] = kSTFSBlocksPerHashLevel[0] + blocks_per_hash_table_;
    block_step_[1] =
        kSTFSBlocksPerHashLevel[1] + ((kSTFSBlocksPerHashLevel[0] + 1) * blocks_per_hash_table_);
  }

  [[nodiscard]] u64 BlockToOffset(u64 block_index) const {
    u64 base = kSTFSBlocksPerHashLevel[0];
    u64 block = block_index;
    for (u32 level = 0; level < 3; ++level) {
      block += ((block_index + base) / base) * blocks_per_hash_table_;
      if (block_index < base) {
        break;
      }
      base *= kSTFSBlocksPerHashLevel[0];
    }
    return DataStart() + (block << 12);
  }

  [[nodiscard]] const STFSHashEntry* GetBlockHash(u32 block_index) {
    const auto& descriptor = header_.metadata.volume_descriptor.stfs;
    u32 secondary = descriptor.flags.bits.root_active_index ? kSTFSBlockSize : 0;
    const u64 offset_lv0 = HashBlockOffset(block_index, 0);
    if (!cache_.contains(offset_lv0)) {
      if (descriptor.flags.bits.read_only_format) {
        secondary = 0;
      } else if (descriptor.total_block_count > kSTFSBlocksPerHashLevel[0]) {
        const u64 offset_lv1 = HashBlockOffset(block_index, 1);
        if (!cache_.contains(offset_lv1)) {
          if (descriptor.total_block_count > kSTFSBlocksPerHashLevel[1]) {
            const u64 offset_lv2 = HashBlockOffset(block_index, 2);
            if (!cache_.contains(offset_lv2) && !Load(offset_lv2, offset_lv2 + secondary)) {
              return nullptr;
            }
            const u32 entry_index =
                (block_index / kSTFSBlocksPerHashLevel[1]) % kSTFSBlocksPerHashLevel[0];
            secondary = cache_[offset_lv2].entries[entry_index].IsLevelNActiveIndexSet()
                            ? kSTFSBlockSize
                            : 0;
          }
          if (!Load(offset_lv1, offset_lv1 + secondary)) {
            return nullptr;
          }
        }
        const u32 entry_index =
            (block_index / kSTFSBlocksPerHashLevel[0]) % kSTFSBlocksPerHashLevel[0];
        secondary =
            cache_[offset_lv1].entries[entry_index].IsLevelNActiveIndexSet() ? kSTFSBlockSize : 0;
      }
      if (!Load(offset_lv0, offset_lv0 + secondary)) {
        return nullptr;
      }
    }
    return &cache_[offset_lv0].entries[block_index % kSTFSBlocksPerHashLevel[0]];
  }

 private:
  [[nodiscard]] u64 DataStart() const {
    return RoundUpNonZero(static_cast<u64>(header_.header.header_size),
                          static_cast<u64>(kSTFSBlockSize));
  }

  [[nodiscard]] u32 HashBlockNumber(u32 block_index, u32 level) const {
    if (level == 0) {
      if (block_index < kSTFSBlocksPerHashLevel[0]) {
        return 0;
      }
      u32 block = (block_index / kSTFSBlocksPerHashLevel[0]) * block_step_[0];
      block += ((block_index / kSTFSBlocksPerHashLevel[1]) + 1) * blocks_per_hash_table_;
      if (block_index < kSTFSBlocksPerHashLevel[1]) {
        return block;
      }
      return block + blocks_per_hash_table_;
    }
    if (level == 1) {
      if (block_index < kSTFSBlocksPerHashLevel[1]) {
        return block_step_[0];
      }
      return ((block_index / kSTFSBlocksPerHashLevel[1]) * block_step_[1]) + blocks_per_hash_table_;
    }
    return block_step_[1];
  }

  [[nodiscard]] u64 HashBlockOffset(u32 block_index, u32 level) const {
    return DataStart() + (static_cast<u64>(HashBlockNumber(block_index, level)) << 12);
  }

  bool Load(u64 key, u64 offset) {
    auto table = ReadAt<STFSHashTable>(file_, offset);
    if (!table) {
      REXFS_FAIL(false, "STFS hash table at {:#x} is outside the package", offset);
    }
    cache_[key] = *table;
    return true;
  }

  const XContentPackageHeader& header_;
  std::span<const u8> file_;
  u32 blocks_per_hash_table_ = 1;
  std::array<u32, 2> block_step_{};
  std::unordered_map<u64, STFSHashTable> cache_;
};

void AppendExtent(ContainerNode& node, u64 offset, u64 length) {
  if (!node.extents.empty()) {
    ContainerExtent& last = node.extents.back();
    if (last.file == 0 && last.offset + last.length == offset) {
      last.length += length;
      return;
    }
  }
  node.extents.push_back({.file = 0, .offset = offset, .length = length});
}

}  // namespace

[[nodiscard]] Result<void> ParseSTFS(const XContentPackageHeader& header, std::span<const u8> file,
                                     std::vector<ContainerNode>& nodes) {
  STFSReader reader(header, file);
  const auto& descriptor = header.metadata.volume_descriptor.stfs;
  const u32 table_blocks = descriptor.file_table_block_count;
  u32 table_block = descriptor.GetFileTableBlockNumber();
  u32 tables_read = 0;
  for (u32 n = 0; n < table_blocks; ++n) {
    const u64 table_offset = reader.BlockToOffset(table_block);
    auto directory = ReadAt<STFSDirectoryBlock>(file, table_offset);
    if (!directory) {
      return Err(Corrupt(
          std::format("STFS directory block at {:#x} is outside the package", table_offset)));
    }
    ++tables_read;
    for (u32 m = 0; m < kEntriesPerDirectoryBlock; ++m) {
      const STFSDirectoryEntry& entry = directory->entries[m];
      if (entry.name[0] == 0) {
        break;
      }
      const u16 directory_index = entry.directory_index;
      u32 parent = 0;
      if (directory_index != kRootDirectoryIndex) {
        parent = static_cast<u32>(directory_index) + 1;
        if (parent >= nodes.size()) {
          return Err(Corrupt(std::format("STFS entry names parent {} of {} entries",
                                         directory_index, nodes.size() - 1)));
        }
      }
      ContainerNode node;
      node.info.name.assign(entry.name,
                            std::min<size_t>(entry.flags.name_length & 0x3F, sizeof(entry.name)));
      node.parent = parent;
      node.info.size = entry.length;
      node.info.allocation_size =
          RoundUpNonZero(static_cast<u64>(entry.length), static_cast<u64>(kSTFSBlockSize));
      node.info.create_time = DecodeFATTimestamp(entry.create_date, entry.create_time);
      node.info.write_time = DecodeFATTimestamp(entry.modified_date, entry.modified_time);
      node.info.access_time = node.info.write_time;
      if (entry.flags.directory) {
        node.info.attributes = FileAttributes::kDirectory;
      } else {
        node.info.attributes = FileAttributes::kNormal | FileAttributes::kReadOnly;
        u32 block = entry.GetStartBlockNumber();
        u64 remaining = entry.length;
        u32 blocks = 0;
        while (remaining && block != kEndOfChain) {
          const u64 length = std::min(static_cast<u64>(kSTFSBlockSize), remaining);
          AppendExtent(node, reader.BlockToOffset(block), length);
          ++blocks;
          remaining -= length;
          const STFSHashEntry* hash = reader.GetBlockHash(block);
          if (!hash) {
            return Err(Corrupt(
                std::format("STFS hash of block {} of '{}' is missing", block, node.info.name)));
          }
          block = hash->GetLevel0NextBlock();
        }
        if (remaining) {
          REXFS_WARN("STFS file {} holds {} of {} bytes", node.info.name,
                     static_cast<u64>(entry.length) - remaining, static_cast<u32>(entry.length));
        }
        if (blocks != entry.GetAllocatedDataBlocks()) {
          REXFS_WARN("STFS file {} chains {} blocks, the entry lists {}", node.info.name, blocks,
                     entry.GetAllocatedDataBlocks());
        }
      }
      const auto index = static_cast<u32>(nodes.size());
      nodes.push_back(std::move(node));
      nodes[parent].children.push_back(index);
    }
    const STFSHashEntry* hash = reader.GetBlockHash(table_block);
    if (!hash) {
      return Err(Corrupt(std::format("STFS hash of directory block {} is missing", table_block)));
    }
    table_block = hash->GetLevel0NextBlock();
    if (table_block == kEndOfChain) {
      break;
    }
  }
  if (tables_read != table_blocks) {
    REXFS_WARN("STFS read {} directory blocks, the header lists {}", tables_read, table_blocks);
  }
  return Ok();
}

}  // namespace rex::filesystem::detail
