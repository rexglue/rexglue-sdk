#include "filesystem/container_provider.h"

#include <format>

#include <rex/types.h>
#include <rex/filesystem/error.h>
#include <rex/result.h>
#include <rex/math.h>
#include <rex/string.h>

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

ContainerProvider::ContainerProvider(Key, MountEntry entry, Container container)
    : Provider(ReadOnly(std::move(entry))), container_(std::move(container)) {}

Result<std::unique_ptr<Provider>> ContainerProvider::Create(const MountEntry& entry) {
  REX_TRY(Container container, Container::Open(entry.source));
  if (!KindMatches(entry.kind, container.GetKind())) {
    return Err<std::unique_ptr<Provider>>(
        BuildError(FileSystemError::kCorrupt,
                   std::format("'{}' does not hold the container kind mount '{}' names",
                               PathToUTF8(entry.source), entry.path)));
  }
  auto provider = std::make_unique<ContainerProvider>(Key{}, entry, std::move(container));
  auto root = std::make_unique<ContainerNodeImpl>(*provider, nullptr, std::string(),
                                                  provider->container_.GetRoot());
  provider->Build(*root, provider->container_.GetRoot());
  provider->root_ = std::move(root);
  return std::unique_ptr<Provider>(std::move(provider));
}

void ContainerProvider::Build(Node& parent, const ContainerNode& source) {
  const std::span<const ContainerNode> nodes = container_.GetNodes();
  for (u32 index : source.children) {
    const ContainerNode& child_source = nodes[index];
    Node* child = parent.AddChild(std::make_unique<ContainerNodeImpl>(
        *this, &parent, utf8_join_guest_paths(parent.GetPath(), child_source.info.name),
        child_source));
    if (!child_source.children.empty()) {
      Build(*child, child_source);
    }
  }
}

VolumeInfo ContainerProvider::GetVolume() const {
  if (container_.GetKind() == ContainerKind::kGDFX) {
    return {
        "GDFX",         0, 255, static_cast<u32>(container_.GetTotalSize() / kBytesPerSector), 0, 1,
        kBytesPerSector};
  }
  const STFSHeader& header = *container_.GetHeader();
  const auto& metadata = header.metadata;
  u32 total_units = 0;
  u32 available_units = 0;
  if (container_.GetKind() == ContainerKind::kSTFS) {
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
    const u64 data_size = container_.GetTotalSize() - header_bytes;
    total_units = static_cast<u32>(data_size / kPackageSectorsPerUnit / kBytesPerSector);
  }
  return {"STFS", 0, 40, total_units, available_units, kPackageSectorsPerUnit, kBytesPerSector};
}

ContainerNodeImpl::ContainerNodeImpl(ContainerProvider& provider, Node* parent, std::string path,
                                     const ContainerNode& source)
    : Node(provider, parent, std::move(path)), source_(source) {
  info_.attributes = source.info.attributes;
  info_.size = source.info.size;
  info_.allocation_size = source.info.allocation_size;
  info_.create_time = source.info.create_time;
  info_.access_time = source.info.access_time;
  info_.write_time = source.info.write_time;
}

Result<std::unique_ptr<NodeStream>> ContainerNodeImpl::Open(Access access) {
  (void)access;
  const auto& container = static_cast<ContainerProvider&>(GetProvider()).GetContainer();
  return std::unique_ptr<NodeStream>(std::make_unique<ContainerStream>(container, source_));
}

}  // namespace rex::filesystem
