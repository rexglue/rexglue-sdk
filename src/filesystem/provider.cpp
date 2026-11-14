#include "filesystem/provider.h"

#include <format>
#include <mutex>
#include <utility>

#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/mount.h>

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
                [](const std::unique_ptr<Node>& node) { return node->GetOpenCount() == 0; });
}

Result<std::unique_ptr<Provider>> CreateProvider(const MountEntry& entry) {
  switch (entry.type) {
    case ProviderType::kHostDirectory:
      return HostProvider::Create(entry);
    case ProviderType::kSTFS:
    case ProviderType::kDiscImage:
      return ContainerProvider::Create(entry);
    case ProviderType::kNull:
      return NullProvider::Create(entry);
  }
  return Err<std::unique_ptr<Provider>>(BuildError(
      FileSystemError::kUnsupported, std::format("mount '{}' names provider type {}", entry.path,
                                                 std::to_underlying(entry.type))));
}

}  // namespace rex::filesystem
