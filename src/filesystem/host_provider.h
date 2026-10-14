/**
 * @file        filesystem/host_provider.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/host_path_device.h and host_path_entry.h
 *              (Ben Vanik, 2020).
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "filesystem/host_file.h"
#include "filesystem/provider.h"

namespace rex::filesystem {

/** Serves a host folder. The tree is read at mount, a missing path is looked up on demand. */
class HostProvider final : public Provider {
  struct Key {};

 public:
  /**
   * Serves the folder of a mount entry. Only Create can make the key.
   *
   * @param key The private construction key.
   * @param entry The mount description.
   */
  explicit HostProvider(Key, MountEntry entry);

  /**
   * Builds the provider and reads the folder tree. A missing folder is created on a writable
   * mount.
   *
   * @param entry The mount description.
   * @return The provider, or kNotFound when the folder is missing on a read only mount.
   */
  [[nodiscard]] static Result<std::unique_ptr<Provider>> Create(const MountEntry& entry);

  /**
   * Returns the volume information, named STFS.
   *
   * @return The name and geometry.
   */
  [[nodiscard]] VolumeInfo GetVolume() const override;

  /**
   * Returns the node at a path, looking it up on the host when the tree lacks it, case
   * insensitive.
   *
   * @param path The relative path.
   * @return The node, or null.
   */
  [[nodiscard]] Node* Resolve(std::string_view path) override;

 private:
};

/** 1 host file or folder. */
class HostNode final : public Node {
 public:
  /**
   * Creates a node without reading the host.
   *
   * @param provider The provider that owns the node.
   * @param parent The parent, or null for the root.
   * @param path The path relative to the mount.
   * @param host_path The host file or folder.
   */
  HostNode(HostProvider& provider, Node* parent, std::string path, std::filesystem::path host_path);

  /**
   * Creates a child node from host information.
   *
   * @param provider The provider that owns the node.
   * @param parent The parent directory.
   * @param info The host information of the child.
   * @return The node.
   */
  [[nodiscard]] static std::unique_ptr<HostNode> Create(HostProvider& provider, HostNode& parent,
                                                        const HostInfo& info);

  /**
   * Returns the host file or folder, read under the provider mutex.
   *
   * @return A copy of the host path.
   */
  [[nodiscard]] std::filesystem::path GetHostPath() const;

  /**
   * Reads the host tree under this folder into child nodes. A folder already on the walk, reached
   * through a link, is listed without its children.
   */
  void Populate();

  [[nodiscard]] Result<std::unique_ptr<NodeStream>> Open(Access access) override;
  [[nodiscard]] Result<void> Truncate() override;
  void Refresh() override;
  [[nodiscard]] bool HasChildVanished(const Node& child) const override;
  [[nodiscard]] Result<void> SetAttributes(FileAttributes attributes) override;
  [[nodiscard]] Result<void> SetTimes(const FileTimes& times) override;

 private:
  [[nodiscard]] Result<std::unique_ptr<Node>> CreateChildImpl(std::string_view name,
                                                              FileAttributes attributes) override;
  [[nodiscard]] Result<void> DeleteChildImpl(Node& child) override;
  [[nodiscard]] Result<void> MoveImpl(Node& new_parent, std::string_view name,
                                      bool replace_existing) override;
  void OnMoved() override;
  void Populate(std::vector<std::pair<u64, u64>>& ancestors);

  [[nodiscard]] HostProvider& GetHost() const { return static_cast<HostProvider&>(GetProvider()); }

  std::filesystem::path host_path_;
};

}  // namespace rex::filesystem
