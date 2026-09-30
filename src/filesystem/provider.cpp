/**
 * @file        filesystem/provider.cpp
 * @brief       The backing of 1 mount and the factory that builds it from a mount entry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/device.cc (Ben Vanik, 2020).
 */

#include "filesystem/provider.h"

#include <format>
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
  return Err<std::unique_ptr<Provider>>(
      MakeError(Error::kUnsupported, std::format("mount '{}' names provider kind {}", entry.path,
                                                 std::to_underlying(entry.kind))));
}

}  // namespace rex::filesystem
