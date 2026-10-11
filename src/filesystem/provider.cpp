#include "filesystem/provider.h"

#include <format>
#include <mutex>
#include <utility>

#include <rex/filesystem/error.h>

#include "filesystem/container_provider.h"
#include "filesystem/host_provider.h"
#include "filesystem/null_provider.h"

namespace rex::filesystem {

Provider::Provider(MountEntry entry) : entry_(std::move(entry)) {}

Provider::~Provider() = default;

Node* Provider::Resolve(std::string_view path) {
  return root_->ResolvePath(path);
}

void Provider::Detach(std::unique_ptr<Node> node) {
  std::lock_guard lock(mutex_);
  detached_.push_back(std::move(node));
}

void Provider::ReleaseDetached() {
  std::lock_guard lock(mutex_);
  std::erase_if(detached_,
                [](const std::unique_ptr<Node>& node) { return node->open_count() == 0; });
}

Result<std::unique_ptr<Provider>> CreateProvider(const MountEntry& entry) {
  switch (entry.kind) {
    case ProviderKind::kHostDirectory:
      return HostProvider::Create(entry);
    case ProviderKind::kSTFS:
    case ProviderKind::kDiscImage:
      return ContainerProvider::Create(entry);
    case ProviderKind::kNull:
      return NullProvider::Create(entry);
  }
  return Err<std::unique_ptr<Provider>>(BuildError(
      FileSystemError::kUnsupported, std::format("mount '{}' names provider kind {}", entry.path,
                                                 std::to_underlying(entry.kind))));
}

}  // namespace rex::filesystem
