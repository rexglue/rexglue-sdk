/**
 * @file        filesystem/container.h
 * @brief       The container parser behind IContainerImage
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/stfs.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::memory {
class MappedMemory;
}  // namespace rex::memory

namespace rex::filesystem {

/**
 * Reads the package header from the first bytes of a package.
 *
 * @param bytes At least sizeof(XContentPackageHeader) bytes from the start of the package.
 * @return The header, or kCorrupt when the bytes are short or the magic is not a package.
 */
[[nodiscard]] Result<XContentPackageHeader> ReadSTFSHeader(std::span<const u8> bytes);

/**
 * Reads the package header of a host file.
 *
 * @param path The package file.
 * @return The header, kNotFound when the file cannot be opened, or kCorrupt as the span form.
 */
[[nodiscard]] Result<XContentPackageHeader> ReadSTFSHeader(const std::filesystem::path& path);

/** A mapped and parsed container image. Move only. */
class Container final : public IContainerImage, public MoveOnly {
 public:
  Container(Container&& other) noexcept;
  Container& operator=(Container&& other) noexcept;
  ~Container() override;

  /**
   * Maps and parses a container. A folder is searched breadth first for a package. SVOD data
   * fragments come from the \<name\>.data folder beside the header file.
   *
   * @param path The container file or a folder that holds one.
   * @return The container, kNotFound when nothing can be opened, kCorrupt when the image fails
   *         validation.
   */
  [[nodiscard]] static Result<Container> Open(const std::filesystem::path& path);

  [[nodiscard]] ContainerType GetType() const override { return type_; }
  [[nodiscard]] const XContentPackageHeader* GetHeader() const override { return header_.get(); }
  [[nodiscard]] const std::filesystem::path& GetPath() const override { return path_; }
  [[nodiscard]] u64 GetTotalSize() const override { return total_size_; }
  [[nodiscard]] std::span<const ContainerNode> GetNodes() const override { return nodes_; }
  [[nodiscard]] const ContainerNode& GetRoot() const override { return nodes_.front(); }
  [[nodiscard]] const ContainerNode* Find(std::string_view path) const override;
  [[nodiscard]] std::string GetNodePath(const ContainerNode& node) const override;
  [[nodiscard]] Result<size_t> Read(const ContainerNode& node, u64 offset,
                                    std::span<u8> buffer) const override;
  [[nodiscard]] std::span<const u8> View(const ContainerNode& node) const override;
  [[nodiscard]] Result<void> Extract(const std::filesystem::path& destination) const override;

 private:
  Container();

  ContainerType type_ = ContainerType::kSTFS;
  std::filesystem::path path_;
  std::unique_ptr<XContentPackageHeader> header_;
  std::vector<std::unique_ptr<memory::MappedMemory>> files_;
  std::vector<ContainerNode> nodes_;
  u64 total_size_ = 0;
};

}  // namespace rex::filesystem
