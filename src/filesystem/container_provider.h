/**
 * @file        filesystem/container_provider.h
 * @brief       The provider that serves an STFS, SVOD or GDFX container read only
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_container_device.h and
 *              disc_image_device.h (Ben Vanik, 2020).
 */

#pragma once

#include <memory>
#include <string>

#include <rex/filesystem/container.h>

#include "filesystem/provider.h"

namespace rex::filesystem {

/** Serves a container image. Every node is read only. */
class ContainerProvider final : public Provider {
 public:
  /**
   * Opens the container and builds the node tree. kWritable is cleared.
   *
   * @param entry The mount description, kSTFS or kDiscImage.
   * @return The provider, the Container::Open error, or kCorrupt when the image kind does not
   *         match the entry kind.
   */
  [[nodiscard]] static Result<std::unique_ptr<Provider>> Create(const MountEntry& entry);

  /**
   * Returns the volume information, named STFS for a package and GDFX for a disc image.
   *
   * @return The name and geometry.
   */
  [[nodiscard]] VolumeInfo Volume() const override;

  /**
   * Returns the parsed image.
   *
   * @return The container.
   */
  [[nodiscard]] const Container& container() const { return container_; }

 private:
  ContainerProvider(MountEntry entry, Container container);

  void Build(Node& parent, const ContainerNode& source);

  Container container_;
};

/** 1 file or directory of a container. */
class ContainerNodeImpl final : public Node {
 public:
  /**
   * Creates a node over a container node.
   *
   * @param provider The provider that owns the node.
   * @param parent The parent, or null for the root.
   * @param path The path relative to the mount.
   * @param source The container node.
   */
  ContainerNodeImpl(ContainerProvider& provider, Node* parent, std::string path,
                    const ContainerNode& source);

  [[nodiscard]] Result<std::unique_ptr<NodeStream>> Open(Access access) override;

 private:
  const ContainerNode& source_;
};

}  // namespace rex::filesystem
