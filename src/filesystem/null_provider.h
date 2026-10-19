/**
 * @file        filesystem/null_provider.h
 * @brief       The provider that answers a fixed list of paths with empty files
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/null_device.h (Ben Vanik, 2020).
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "filesystem/provider.h"

namespace rex::filesystem {

/**
 * Answers the null paths of its mount entry. Every read and write succeeds with 0 bytes, so title
 * code that opens raw partitions finds them present.
 */
class NullProvider final : public Provider {
  struct Key {};

 public:
  /**
   * Serves the null paths of a mount entry. Only Create can make the key.
   *
   * @param key The private construction key.
   * @param entry The mount description.
   */
  explicit NullProvider(Key, MountEntry entry);

  /**
   * Builds the provider with 1 file per null path.
   *
   * @param entry The mount description.
   * @return The provider.
   */
  [[nodiscard]] static Result<std::unique_ptr<Provider>> Create(const MountEntry& entry);

  /**
   * Returns the volume information, named NullDevice.
   *
   * @return The name and geometry.
   */
  [[nodiscard]] VolumeInfo GetVolume() const override;

  /**
   * Returns the root for an empty path or the null file whose path matches, case insensitive.
   *
   * @param path The relative path.
   * @return The node, or null.
   */
  [[nodiscard]] Node* Resolve(std::string_view path) override;

 private:
};

/** The root or a null file. */
class NullNode final : public Node {
 public:
  /**
   * Creates a node.
   *
   * @param provider The provider that owns the node.
   * @param parent The parent, or null for the root.
   * @param path The path relative to the mount.
   */
  NullNode(NullProvider& provider, Node* parent, std::string path);

  [[nodiscard]] Result<std::unique_ptr<NodeStream>> Open(Access access) override;
};

}  // namespace rex::filesystem
