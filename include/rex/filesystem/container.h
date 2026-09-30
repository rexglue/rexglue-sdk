/**
 * @file        filesystem/container.h
 * @brief       STFS, SVOD and GDFX container images parsed into a file tree
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/devices/stfs_container_device.cc and
 *              disc_image_device.cc (Ben Vanik, 2020).
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/info.h>
#include <rex/filesystem/xcontent.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::memory {
class MappedMemory;
}  // namespace rex::memory

namespace rex::filesystem {

/** Format of a container image. */
enum class ContainerKind : u32 {
  kSTFS, /**< STFS package, 1 file. */
  kSVOD, /**< SVOD package, a header file and data fragments. */
  kGDFX, /**< GDFX disc image. */
};

/** 1 contiguous run of file bytes inside a container data file. */
struct ContainerExtent {
  u32 file = 0;   /**< Index of the data file. */
  u64 offset = 0; /**< Byte offset in that file. */
  u64 length = 0; /**< Byte count. */
};

/** 1 file or directory of a container. */
struct ContainerNode {
  std::string name;                                  /**< Name, empty for the root. */
  u32 parent = 0;                                    /**< Index of the parent, 0 for the root. */
  FileAttributes attributes = FileAttributes::kNone; /**< Attribute bits. */
  u64 size = 0;                                      /**< Size in bytes. */
  u64 allocation_size = 0;                           /**< Size rounded up to the block size. */
  u64 create_time = 0;                               /**< Creation time in FILETIME ticks. */
  u64 access_time = 0;                               /**< Last access time in FILETIME ticks. */
  u64 write_time = 0;                                /**< Last write time in FILETIME ticks. */
  std::vector<u32> children;                         /**< Indices of the children in order. */
  std::vector<ContainerExtent> extents; /**< File bytes in order, empty for a directory. */
};

/** A mapped and parsed container image. Move only. */
class REX_API Container : public MoveOnly {
 public:
  Container(Container&& other) noexcept;
  Container& operator=(Container&& other) noexcept;
  ~Container();

  /**
   * Maps and parses a container. A folder is searched breadth first for the first file whose
   * magic is a package. The kind comes from the magic, the SVOD data fragments from the
   * <name>.data folder beside the header file.
   *
   * @param path The container file or a folder that holds 1.
   * @return The container, kNotFound when nothing can be opened, kCorrupt when the image fails
   *         validation.
   */
  [[nodiscard]] static Result<Container> Open(const std::filesystem::path& path);

  /**
   * Returns the image format.
   *
   * @return The kind.
   */
  [[nodiscard]] ContainerKind kind() const { return kind_; }

  /**
   * Returns the package header.
   *
   * @return The header of an STFS or SVOD package, null for a GDFX image.
   */
  [[nodiscard]] const STFSHeader* header() const { return header_.get(); }

  /**
   * Returns the host file that was opened.
   *
   * @return The package file or disc image.
   */
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  /**
   * Returns the bytes of every mapped data file together.
   *
   * @return The total size in bytes.
   */
  [[nodiscard]] u64 total_size() const { return total_size_; }

  /**
   * Returns every node, the root first.
   *
   * @return The nodes.
   */
  [[nodiscard]] std::span<const ContainerNode> nodes() const { return nodes_; }

  /**
   * Returns the root directory.
   *
   * @return The root node.
   */
  [[nodiscard]] const ContainerNode& root() const { return nodes_.front(); }

  /**
   * Returns the node at a path.
   *
   * @param path Components split by a backslash or a slash, case insensitive, empty for the root.
   * @return The node, or null when no node has the path.
   */
  [[nodiscard]] const ContainerNode* Find(std::string_view path) const;

  /**
   * Returns the path of a node.
   *
   * @param node A node of this container.
   * @return The components from the root joined with a backslash, empty for the root.
   */
  [[nodiscard]] std::string PathOf(const ContainerNode& node) const;

  /**
   * Copies bytes of a file node.
   *
   * @param node A file node of this container.
   * @param offset Byte offset in the file.
   * @param buffer Destination. At most buffer.size() bytes are copied.
   * @return The bytes copied, or kEndOfFile when offset is at or past the end of the file.
   */
  [[nodiscard]] Result<size_t> Read(const ContainerNode& node, u64 offset,
                                    std::span<u8> buffer) const;

  /**
   * Returns the bytes of a file node without a copy.
   *
   * @param node A file node of this container.
   * @return The bytes, valid while the container lives, empty when the file is not 1 extent.
   */
  [[nodiscard]] std::span<const u8> View(const ContainerNode& node) const;

  /**
   * Writes every node under a host folder. A node name that is empty, dot, dot dot, or holds a
   * separator or a colon is refused. What was written is removed when any step fails.
   *
   * @param destination The host folder, created when missing.
   * @return kInvalidPath for a refused name, kIO for a host failure.
   */
  [[nodiscard]] Result<void> Extract(const std::filesystem::path& destination) const;

 private:
  Container();

  ContainerKind kind_ = ContainerKind::kSTFS;
  std::filesystem::path path_;
  std::unique_ptr<STFSHeader> header_;
  std::vector<std::unique_ptr<memory::MappedMemory>> files_;
  std::vector<ContainerNode> nodes_;
  u64 total_size_ = 0;
};

}  // namespace rex::filesystem
