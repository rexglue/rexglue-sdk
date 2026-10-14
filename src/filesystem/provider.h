/**
 * @file        filesystem/provider.h
 * @brief       The backing of 1 mount and the factory that builds it from a mount entry
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/device.h (Ben Vanik, 2020).
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/info.h>
#include <rex/filesystem/mount.h>
#include <rex/result.h>
#include <rex/types.h>

#include "filesystem/node.h"

namespace rex::filesystem {

/**
 * The backing of 1 mount. Owns the node tree and the mutex its changes take. Open files and walks
 * share ownership, so an unmount frees the tree at the last close.
 */
class Provider : public NonCopyable, public std::enable_shared_from_this<Provider> {
 public:
  virtual ~Provider();

  /**
   * Returns the mount description.
   *
   * @return The entry the provider was built from.
   */
  [[nodiscard]] const MountEntry& GetEntry() const { return entry_; }

  /**
   * Returns the mount path.
   *
   * @return The guest mount path.
   */
  [[nodiscard]] const std::string& GetMountPath() const { return entry_.path; }

  /**
   * Reports whether the mount refuses writes.
   *
   * @return True when kWritable is clear.
   */
  [[nodiscard]] bool IsReadOnly() const { return !HasFlags(entry_.flags, MountFlags::kWritable); }

  /**
   * Returns the volume information titles see.
   *
   * @return The name and geometry.
   */
  [[nodiscard]] virtual VolumeInfo GetVolume() const = 0;

  /**
   * Returns the node at a path relative to the mount.
   *
   * @param path The relative path, empty for the root.
   * @return The node, or null.
   */
  [[nodiscard]] virtual Node* Resolve(std::string_view path);

  /**
   * Returns the root directory.
   *
   * @return The root node.
   */
  [[nodiscard]] Node& GetRoot() const { return *root_; }

  /**
   * Returns the mutex tree changes take.
   *
   * @return The provider mutex.
   */
  [[nodiscard]] std::recursive_mutex& GetMutex() const { return mutex_; }

  /**
   * Keeps a node that left the tree while files are open on it.
   *
   * @param node The node, with open files.
   */
  void Detach(std::unique_ptr<Node> node);

  /** Frees the detached nodes with no open file left. */
  void ReleaseDetached();

 protected:
  /**
   * Stores the mount description.
   *
   * @param entry The mount description.
   */
  explicit Provider(MountEntry entry);

  std::unique_ptr<Node> root_; /**< Root directory, built by the provider. */

 private:
  MountEntry entry_;
  mutable std::recursive_mutex mutex_;
  std::vector<std::unique_ptr<Node>> detached_;
};

/**
 * Builds the provider a mount entry names.
 *
 * @param entry The mount description.
 * @return The provider, kNotFound for a missing source, kCorrupt for a container that fails
 *         validation.
 */
[[nodiscard]] Result<std::unique_ptr<Provider>> CreateProvider(const MountEntry& entry);

}  // namespace rex::filesystem
