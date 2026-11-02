#include "filesystem/container_provider.h"

#include <format>

#include <rex/types.h>
#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/mount.h>
#include <rex/result.h>
#include <rex/math.h>
#include <rex/string/text.h>
#include <rex/filesystem/path.h>

namespace rex::filesystem {

namespace {

constexpr u32 kPackageSectorsPerUnit = 8;

class ContainerStream final : public NodeStream {
 public:
  ContainerStream(const Container& container, const ContainerNode& node)
      : container_(container), node_(node) {}

  [[nodiscard]] Result<size_t> Read(std::span<u8> buffer, u64 offset) override {
    return container_.Read(node_, offset, buffer);
  }

  [[nodiscard]] Result<std::span<const u8>> View() override {
    const std::span<const u8> bytes = container_.View(node_);
    if (bytes.empty() && node_.info.size != 0) {
      return Err<std::span<const u8>>(BuildError(
          FileSystemError::kUnsupported, std::format("'{}' is not 1 extent", node_.info.name)));
    }
    return bytes;
  }

 private:
  const Container& container_;
  const ContainerNode& node_;
};

bool TypeMatches(ProviderType type, ContainerType found) {
  if (type == ProviderType::kDiscImage) {
    return found == ContainerType::kGDFX;
  }
  return found == ContainerType::kSTFS || found == ContainerType::kSVOD;
}

MountEntry MakeReadOnly(MountEntry entry) {
  entry.flags &= ~MountFlags::kWritable;
  return entry;
}

}  // namespace

ContainerProvider::ContainerProvider(Key, MountEntry entry, Container container)
    : Provider(MakeReadOnly(std::move(entry))), container_(std::move(container)) {}

Result<std::unique_ptr<Provider>> ContainerProvider::Create(const MountEntry& entry) {
  REX_TRY(Container container, Container::Open(entry.source));
  if (!TypeMatches(entry.type, container.GetType())) {
    return Err<std::unique_ptr<Provider>>(
        BuildError(FileSystemError::kCorrupt,
                   std::format("'{}' does not hold the container type mount '{}' names",
                               string::PathToUTF8(entry.source), entry.path)));
  }
  auto provider = std::make_unique<ContainerProvider>(Key{}, entry, std::move(container));
  auto root = std::make_unique<ContainerProviderNode>(*provider, nullptr, std::string(),
                                                      provider->container_.GetRoot());
  provider->Build(*root, provider->container_.GetRoot());
  provider->root_ = std::move(root);
  return std::unique_ptr<Provider>(std::move(provider));
}

void ContainerProvider::Build(Node& parent, const ContainerNode& source) {
  const std::span<const ContainerNode> nodes = container_.GetNodes();
  for (u32 index : source.children) {
    const ContainerNode& child_source = nodes[index];
    Node* child = parent.AddChild(std::make_unique<ContainerProviderNode>(
        *this, &parent, utf8_join_guest_paths(parent.GetPath(), child_source.info.name),
        child_source));
    if (!child_source.children.empty()) {
      Build(*child, child_source);
    }
  }
}

VolumeStat ContainerProvider::GetVolume() const {
  if (container_.GetType() == ContainerType::kGDFX) {
    return {
        "GDFX",         0, 255, static_cast<u32>(container_.GetTotalSize() / kBytesPerSector), 0, 1,
        kBytesPerSector};
  }
  const XContentPackageHeader& header = *container_.GetHeader();
  const auto& metadata = header.metadata;
  u32 total_units = 0;
  u32 available_units = 0;
  if (container_.GetType() == ContainerType::kSTFS) {
    const auto& descriptor = metadata.volume_descriptor.stfs;
    const u32 total_blocks = memory::load<be_u32>(&descriptor.total_block_count);
    const u32 free_blocks = memory::load<be_u32>(&descriptor.free_block_count);
    total_units = total_blocks;
    if (!descriptor.flags.bits.read_only_format) {
      available_units = kSTFSMaxBlocks - (total_blocks - free_blocks);
    }
  } else {
    const u32 header_size = memory::load<be_u32>(&header.header.header_size);
    const u64 header_bytes = header_size ? RoundUpNonZero(u64{header_size}, u64{kSTFSBlockSize})
                                         : u64{sizeof(XContentPackageHeader)};
    const u64 data_size = container_.GetTotalSize() - header_bytes;
    total_units = static_cast<u32>(data_size / kPackageSectorsPerUnit / kBytesPerSector);
  }
  return {"STFS", 0, 40, total_units, available_units, kPackageSectorsPerUnit, kBytesPerSector};
}

ContainerProviderNode::ContainerProviderNode(ContainerProvider& provider, Node* parent,
                                             std::string path, const ContainerNode& source)
    : Node(provider, parent, std::move(path)), source_(source) {
  info_.attributes = source.info.attributes;
  info_.size = source.info.size;
  info_.allocation_size = source.info.allocation_size;
  info_.create_time = source.info.create_time;
  info_.access_time = source.info.access_time;
  info_.write_time = source.info.write_time;
}

Result<std::unique_ptr<NodeStream>> ContainerProviderNode::Open(Access access) {
  (void)access;
  const auto& container = static_cast<ContainerProvider&>(GetProvider()).GetContainer();
  return std::unique_ptr<NodeStream>(std::make_unique<ContainerStream>(container, source_));
}

}  // namespace rex::filesystem
