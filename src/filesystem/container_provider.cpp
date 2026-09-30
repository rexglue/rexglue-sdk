/**
 * @file        filesystem/container_provider.cpp
 * @brief       The provider that serves an STFS, SVOD or GDFX container read only
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_container_device.cc,
 * stfs_container_file.cc, disc_image_device.cc and disc_image_file.cc (Ben Vanik, 2020).
 */

#include "filesystem/container_provider.h"

#include <format>

#include <rex/filesystem/error.h>
#include <rex/macros.h>
#include <rex/math.h>
#include <rex/string.h>

namespace rex::filesystem {

namespace {

constexpr u32 kSTFSBlockSize = 0x1000;
constexpr u32 kSTFSMaxBlocks = 4913000;
constexpr u32 kBytesPerSector = 0x200;
constexpr u32 kPackageSectorsPerUnit = 8;

class ContainerStream final : public NodeStream {
 public:
  ContainerStream(const Container& container, const ContainerNode& node)
      : container_(container), node_(node) {}

  Result<size_t> Read(std::span<u8> buffer, u64 offset) override {
    return container_.Read(node_, offset, buffer);
  }

  Result<std::span<const u8>> View() override {
    const std::span<const u8> bytes = container_.View(node_);
    if (bytes.empty() && node_.size != 0) {
      return Err<std::span<const u8>>(
          MakeError(Error::kUnsupported, std::format("'{}' is not 1 extent", node_.name)));
    }
    return bytes;
  }

 private:
  const Container& container_;
  const ContainerNode& node_;
};

bool KindMatches(ProviderKind kind, ContainerKind found) {
  if (kind == ProviderKind::kDiscImage) {
    return found == ContainerKind::kGDFX;
  }
  return found == ContainerKind::kSTFS || found == ContainerKind::kSVOD;
}

MountEntry ReadOnly(MountEntry entry) {
  entry.flags &= ~MountFlags::kWritable;
  return entry;
}

}  // namespace

ContainerProvider::ContainerProvider(MountEntry entry, Container container)
    : Provider(ReadOnly(std::move(entry))), container_(std::move(container)) {}

Result<std::unique_ptr<Provider>> ContainerProvider::Create(const MountEntry& entry) {
  REX_TRY(Container container, Container::Open(entry.source));
  if (!KindMatches(entry.kind, container.kind())) {
    return Err<std::unique_ptr<Provider>>(MakeError(
        Error::kCorrupt, std::format("'{}' does not hold the container kind mount '{}' names",
                                     path_to_utf8(entry.source), entry.path)));
  }
  std::unique_ptr<ContainerProvider> provider(new ContainerProvider(entry, std::move(container)));
  auto root = std::make_unique<ContainerNodeImpl>(*provider, nullptr, std::string(),
                                                  provider->container_.root());
  provider->Build(*root, provider->container_.root());
  provider->root_ = std::move(root);
  return std::unique_ptr<Provider>(std::move(provider));
}

void ContainerProvider::Build(Node& parent, const ContainerNode& source) {
  const std::span<const ContainerNode> nodes = container_.nodes();
  for (u32 index : source.children) {
    const ContainerNode& child_source = nodes[index];
    Node* child = parent.AddChild(std::make_unique<ContainerNodeImpl>(
        *this, &parent, utf8_join_guest_paths(parent.path(), child_source.name), child_source));
    if (!child_source.children.empty()) {
      Build(*child, child_source);
    }
  }
}

VolumeInfo ContainerProvider::Volume() const {
  if (container_.kind() == ContainerKind::kGDFX) {
    return {
        "GDFX",         0, 255, static_cast<u32>(container_.total_size() / kBytesPerSector), 0, 1,
        kBytesPerSector};
  }
  const STFSHeader& header = *container_.header();
  const auto& metadata = header.metadata;
  u32 total_units = 0;
  u32 available_units = 0;
  if (container_.kind() == ContainerKind::kSTFS) {
    const auto& descriptor = metadata.volume_descriptor.stfs;
    total_units = descriptor.total_block_count;
    if (!descriptor.flags.bits.read_only_format) {
      available_units =
          kSTFSMaxBlocks - (descriptor.total_block_count - descriptor.free_block_count);
    }
  } else {
    const u64 header_bytes = header.header.header_size
                                 ? round_up(u64{header.header.header_size}, u64{kSTFSBlockSize})
                                 : u64{sizeof(STFSHeader)};
    const u64 data_size = container_.total_size() - header_bytes;
    total_units = static_cast<u32>(data_size / kPackageSectorsPerUnit / kBytesPerSector);
  }
  return {"STFS", 0, 40, total_units, available_units, kPackageSectorsPerUnit, kBytesPerSector};
}

ContainerNodeImpl::ContainerNodeImpl(ContainerProvider& provider, Node* parent, std::string path,
                                     const ContainerNode& source)
    : Node(provider, parent, std::move(path)), source_(source) {
  attributes_ = source.attributes;
  size_ = source.size;
  allocation_size_ = source.allocation_size;
  create_time_ = source.create_time;
  access_time_ = source.access_time;
  write_time_ = source.write_time;
}

Result<std::unique_ptr<NodeStream>> ContainerNodeImpl::Open(Access access) {
  (void)access;
  const auto& container = static_cast<ContainerProvider&>(provider()).container();
  return std::unique_ptr<NodeStream>(std::make_unique<ContainerStream>(container, source_));
}

}  // namespace rex::filesystem
