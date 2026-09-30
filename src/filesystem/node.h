/**
 * @file        filesystem/node.h
 * @brief       The shared tree node of a mount and the byte stream of 1 open node
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/entry.h and file.h (Ben Vanik, 2020).
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/filesystem/info.h>
#include <rex/filesystem/open.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

class Provider;
class WildcardMatcher;

/**
 * Tests whether access allows reading.
 *
 * @param access The open access bits.
 * @return True when a generic read or read data bit is set.
 */
[[nodiscard]] bool CanRead(Access access);

/**
 * Tests whether access allows writing.
 *
 * @param access The open access bits.
 * @return True when a generic write, write data or append data bit is set.
 */
[[nodiscard]] bool CanWrite(Access access);

/**
 * Tests whether access allows a size change.
 *
 * @param access The open access bits.
 * @return True when a generic write or write data bit is set.
 */
[[nodiscard]] bool CanSetLength(Access access);

/** Byte access of 1 open node, served by its provider. */
class NodeStream {
 public:
  virtual ~NodeStream() = default;

  /**
   * Copies bytes of the node.
   *
   * @param buffer Destination.
   * @param offset Byte offset in the node.
   * @return The bytes copied, or the error.
   */
  [[nodiscard]] virtual Result<size_t> Read(std::span<u8> buffer, u64 offset) = 0;

  /**
   * Writes bytes into the node.
   *
   * @param buffer Source bytes.
   * @param offset Byte offset in the node.
   * @return The bytes written, or kAccessDenied when the provider has no writes.
   */
  [[nodiscard]] virtual Result<size_t> Write(std::span<const u8> buffer, u64 offset);

  /**
   * Changes the node size.
   *
   * @param length The new size in bytes.
   * @return kUnsupported when the provider has no size change.
   */
  [[nodiscard]] virtual Result<void> SetLength(u64 length);

  /**
   * Returns the whole node as read only bytes.
   *
   * @return The bytes, or kUnsupported.
   */
  [[nodiscard]] virtual Result<std::span<const u8>> View();
};

/**
 * 1 file or directory of a mount. Every open handle on the path shares the node. Tree changes take
 * the provider mutex.
 */
class Node : public NonCopyable {
 public:
  virtual ~Node();

  /**
   * Returns the provider that owns the node.
   *
   * @return The provider.
   */
  [[nodiscard]] Provider& provider() const { return *provider_; }

  /**
   * Returns the parent directory.
   *
   * @return The parent, or null for the root.
   */
  [[nodiscard]] Node* parent() const { return parent_; }

  /**
   * Returns the last path component.
   *
   * @return The name, empty for the root.
   */
  [[nodiscard]] const std::string& name() const { return name_; }

  /**
   * Returns the path relative to the mount.
   *
   * @return The path.
   */
  [[nodiscard]] const std::string& path() const { return path_; }

  /**
   * Returns the mount path joined with the path.
   *
   * @return The absolute guest path.
   */
  [[nodiscard]] const std::string& absolute_path() const { return absolute_path_; }

  /**
   * Returns the attribute bits.
   *
   * @return The attributes.
   */
  [[nodiscard]] FileAttributes attributes() const { return attributes_; }

  /**
   * Tests whether the node is a directory.
   *
   * @return True when kDirectory is set.
   */
  [[nodiscard]] bool is_directory() const {
    return HasFlags(attributes_, FileAttributes::kDirectory);
  }

  /**
   * Tests whether the mount of the node refuses writes.
   *
   * @return True on a read only mount.
   */
  [[nodiscard]] bool read_only() const;

  /**
   * Tests whether a delete on close request is recorded.
   *
   * @return True when requested.
   */
  [[nodiscard]] bool delete_on_close() const { return delete_on_close_; }

  /**
   * Records or withdraws a delete on close request.
   *
   * @param delete_on_close True to request.
   */
  void set_delete_on_close(bool delete_on_close) { delete_on_close_ = delete_on_close; }

  /**
   * Returns the node information.
   *
   * @return The name, paths, attributes, sizes and times.
   */
  [[nodiscard]] FileInfo Info() const;

  /**
   * Returns the children in insertion order.
   *
   * @return The children.
   */
  [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

  /**
   * Returns the child with a name, case insensitive.
   *
   * @param name The child name.
   * @return The child, or null.
   */
  [[nodiscard]] Node* GetChild(std::string_view name);

  /**
   * Walks a relative path 1 component at a time.
   *
   * @param path Components split by a backslash, empty for this node.
   * @return The node, or null when a component is missing.
   */
  [[nodiscard]] Node* ResolvePath(std::string_view path);

  /**
   * Returns the next child at or after index that matches.
   *
   * @param matcher The name pattern.
   * @param index The child index to start at, advanced past the returned child.
   * @return The child, or null when none is left.
   */
  [[nodiscard]] Node* IterateChildren(const WildcardMatcher& matcher, size_t* index);

  /**
   * Adds a child a provider built. No host change is made.
   *
   * @param child The child.
   * @return The added child.
   */
  Node* AddChild(std::unique_ptr<Node> child);

  /**
   * Removes a child from the tree without a host change, for a child whose backing is gone.
   *
   * @param child A child of this node.
   */
  void DropChild(Node& child);

  /**
   * Creates a child through the provider.
   *
   * @param name The child name.
   * @param attributes kDirectory for a directory, kNormal for a file.
   * @return The child, kAccessDenied on a read only mount or when the provider refuses, kExists.
   */
  [[nodiscard]] Result<Node*> CreateChild(std::string_view name, FileAttributes attributes);

  /**
   * Deletes a child through the provider.
   *
   * @param child A child of this node.
   * @return kAccessDenied on a read only mount or when the provider refuses.
   */
  [[nodiscard]] Result<void> DeleteChild(Node& child);

  /**
   * Deletes this node from its parent.
   *
   * @return kAccessDenied for the root or when the parent refuses.
   */
  [[nodiscard]] Result<void> Delete();

  /**
   * Moves the node through the provider. The first component of path is dropped and the rest is
   * relative to the mount.
   *
   * @param path The new guest path.
   * @return The provider error.
   */
  [[nodiscard]] Result<void> Rename(std::string_view path);

  /**
   * Cuts the file to 0 bytes.
   *
   * @return kUnsupported when the provider cannot truncate.
   */
  [[nodiscard]] virtual Result<void> Truncate();

  /** Reads the size again from the provider. */
  virtual void Refresh();

  /**
   * Tests whether the provider lost a cached child.
   *
   * @param child A child of this node.
   * @return True when the backing file of child is gone.
   */
  [[nodiscard]] virtual bool ChildVanished(const Node& child) const;

  /**
   * Opens the node.
   *
   * @param access The open access bits.
   * @return The stream, or the provider error.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<NodeStream>> Open(Access access) = 0;

  /**
   * Replaces the attribute bits.
   *
   * @param attributes The new bits.
   * @return kUnsupported when the provider has no attribute change.
   */
  [[nodiscard]] virtual Result<void> SetAttributes(FileAttributes attributes);

  /**
   * Changes the times. An empty field keeps its time.
   *
   * @param times The new times.
   * @return kUnsupported when the provider has no time change.
   */
  [[nodiscard]] virtual Result<void> SetTimes(const FileTimes& times);

 protected:
  /**
   * Creates a node.
   *
   * @param provider The provider that owns the node.
   * @param parent The parent, or null for the root.
   * @param path The path relative to the mount.
   */
  Node(Provider& provider, Node* parent, std::string path);

  /**
   * Creates the backing of a child.
   *
   * @param name The child name.
   * @param attributes kDirectory for a directory, kNormal for a file.
   * @return The child, or kUnsupported.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<Node>> CreateChildImpl(std::string_view name,
                                                                      FileAttributes attributes);

  /**
   * Deletes the backing of a child.
   *
   * @param child A child of this node.
   * @return Ok, or kUnsupported.
   */
  [[nodiscard]] virtual Result<void> DeleteChildImpl(Node& child);

  /**
   * Moves the backing of this node.
   *
   * @param parts The new path components relative to the mount.
   * @return Ok, or kUnsupported.
   */
  [[nodiscard]] virtual Result<void> RenameImpl(std::span<const std::string_view> parts);

  std::string name_;                                  /**< Last path component. */
  std::string path_;                                  /**< Path relative to the mount. */
  std::string absolute_path_;                         /**< Mount path joined with path_. */
  FileAttributes attributes_ = FileAttributes::kNone; /**< Attribute bits. */
  u64 size_ = 0;                                      /**< Size in bytes. */
  u64 allocation_size_ = 0;                           /**< Size rounded up to the sector size. */
  u64 create_time_ = 0;                               /**< Creation time in FILETIME ticks. */
  u64 access_time_ = 0;                               /**< Last access time in FILETIME ticks. */
  u64 write_time_ = 0;                                /**< Last write time in FILETIME ticks. */
  std::vector<std::unique_ptr<Node>> children_;       /**< Children in insertion order. */

 private:
  Provider* provider_;
  Node* parent_;
  bool delete_on_close_ = false;
};

}  // namespace rex::filesystem
