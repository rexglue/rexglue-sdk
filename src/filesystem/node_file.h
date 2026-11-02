/**
 * @file        filesystem/node_file.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/file.h (Ben Vanik, 2015).
 */

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/directory.h>
#include <rex/filesystem/file.h>

#include "filesystem/node.h"
#include "filesystem/wildcard.h"

namespace rex::filesystem {

class NodeFile final : public IFile {
 public:
  /**
   * Wraps an open stream of a node.
   *
   * @param node The node. The file keeps it and its provider alive.
   * @param access The access bits, generic bits expanded.
   * @param stream The provider stream.
   */
  NodeFile(Node& node, Access access, std::unique_ptr<NodeStream> stream);

  /** Closes the stream, then releases the node, which deletes it when delete on close is set. */
  ~NodeFile() override;

  [[nodiscard]] Node& GetNode() const { return *node_; }

  [[nodiscard]] FileStat GetInfo() const override;
  void Refresh() override;
  [[nodiscard]] Access GetAccess() const override { return access_; }
  [[nodiscard]] VolumeStat GetVolume() const override;
  [[nodiscard]] Result<size_t> Read(std::span<u8> buffer, u64 offset) override;
  [[nodiscard]] Result<size_t> Write(std::span<const u8> buffer, u64 offset) override;
  [[nodiscard]] Result<void> SetLength(u64 length) override;
  [[nodiscard]] Result<void> Flush() override;
  [[nodiscard]] Result<void> SetAttributes(FileAttributes attributes) override;
  [[nodiscard]] Result<void> SetTimes(const FileTimes& times) override;
  void SetDeleteOnClose(bool delete_on_close) override;
  [[nodiscard]] Result<std::unique_ptr<IDirectory>> List(std::string_view pattern) override;
  [[nodiscard]] Result<std::span<const u8>> View() override;

 private:
  std::shared_ptr<Provider> provider_;
  Node* node_;
  Access access_;
  std::unique_ptr<NodeStream> stream_;
};

/** A walk of the children of a directory node. */
class NodeDirectory final : public IDirectory {
 public:
  /**
   * Starts a walk.
   *
   * @param node The directory. The walk keeps it and its provider alive.
   * @param pattern The wildcard pattern, empty for every child.
   */
  NodeDirectory(Node& node, std::string_view pattern);

  ~NodeDirectory() override;

  [[nodiscard]] std::optional<FileStat> Next() override;
  void Restart() override { index_ = 0; }
  [[nodiscard]] u64 GetPosition() const override { return index_; }

 private:
  std::shared_ptr<Provider> provider_;
  Node* node_;
  WildcardMatcher matcher_;
  size_t index_ = 0;
};

}  // namespace rex::filesystem
