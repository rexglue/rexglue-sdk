/**
 * @file        filesystem/container_provider.h
 * @brief       Read only provider for STFS, SVOD and GDFX containers
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

#include "filesystem/container.h"

#include "filesystem/provider.h"

namespace rex::filesystem {

/** Serves a container image read only. */
class ContainerProvider final : public Provider {
  struct Key {};

 public:
  ContainerProvider(Key, MountEntry entry, Container container);

  /**
   * Opens the container and builds the node tree. kWritable is cleared.
   *
   * @param entry The mount description, kSTFS or kDiscImage.
   * @return The provider, the Container::Open error, or kCorrupt when the image type does not
   *         match the entry type.
   */
  [[nodiscard]] static Result<std::unique_ptr<Provider>> Create(const MountEntry& entry);

  /** Returns the volume information, named STFS for a package and GDFX for a disc image. */
  [[nodiscard]] VolumeStat GetVolume() const override;

  [[nodiscard]] const Container& GetContainer() const { return container_; }

 private:
  void Build(Node& parent, const ContainerNode& source);

  Container container_;
};

/** A file or directory of a container. */
class ContainerProviderNode final : public Node {
 public:
  ContainerProviderNode(ContainerProvider& provider, Node* parent, std::string path,
                        const ContainerNode& source);

  [[nodiscard]] Result<std::unique_ptr<NodeStream>> Open(Access access) override;

 private:
  const ContainerNode& source_;
};

}  // namespace rex::filesystem
