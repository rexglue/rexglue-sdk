/**
 * @file        filesystem/node.h
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
 * Reports whether access allows reading.
 *
 * @param access The open access bits.
 * @return True when a generic read or read data bit is set.
 */
[[nodiscard]] bool CanRead(Access access);

/**
 * Reports whether access allows writing.
 *
 * @param access The open access bits.
 * @return True when a generic write, write data or append data bit is set.
 */
[[nodiscard]] bool CanWrite(Access access);

/**
 * Reports whether access allows a size change.
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
   * Writes buffered data of the node to its device.
   *
   * @return Ok when the provider has nothing to write, or the error.
   */
  [[nodiscard]] virtual Result<void> Flush();

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
  [[nodiscard]] Provider& GetProvider() const { return *provider_; }

  /**
   * Returns the parent directory.
   *
   * @return The parent, or null for the root.
   */
  [[nodiscard]] Node* GetParent() const { return parent_; }

  /**
   * Returns the last path component.
   *
   * @return The name, empty for the root.
   */
  [[nodiscard]] const std::string& GetName() const { return name_; }

  /**
   * Returns the path relative to the mount.
   *
   * @return The path.
   */
  [[nodiscard]] const std::string& GetPath() const { return path_; }

  /**
   * Returns the mount path joined with the path.
   *
   * @return The absolute guest path.
   */
  [[nodiscard]] const std::string& GetAbsolutePath() const { return absolute_path_; }

  /**
   * Returns the attribute bits.
   *
   * @return The attributes.
   */
  [[nodiscard]] FileAttributes GetAttributes() const { return attributes_; }

  /**
   * Reports whether the node is a directory.
   *
   * @return True when kDirectory is set.
   */
  [[nodiscard]] bool IsDirectory() const {
    return HasFlags(attributes_, FileAttributes::kDirectory);
  }

  /**
   * Reports whether the mount of the node refuses writes.
   *
   * @return True on a read only mount.
   */
  [[nodiscard]] bool IsReadOnly() const;

  /**
   * Reports whether a delete on close request is set.
   *
   * @return True when requested.
   */
  [[nodiscard]] bool DeleteOnClose() const;

  /**
   * Sets or withdraws a delete on close request.
   *
   * @param delete_on_close True to request.
   */
  void SetDeleteOnClose(bool delete_on_close);

  /**
   * Returns the count of open files and walks on the node and its descendants.
   *
   * @return The count.
   */
  [[nodiscard]] u32 GetOpenCount() const { return open_count_; }

  /** Counts 1 open file or walk on the node and its ancestors. */
  void AddOpen();

  /**
   * Uncounts 1 open file or walk. At the last close the node is deleted when delete on close is
   * set, and a detached node is freed. The node may be gone when this returns.
   */
  void ReleaseOpen();

  /**
   * Returns the node information.
   *
   * @return The name, paths, attributes, sizes and times.
   */
  [[nodiscard]] FileInfo GetInfo() const;

  /**
   * Returns the children in insertion order.
   *
   * @return The children.
   */
  [[nodiscard]] const std::vector<std::unique_ptr<Node>>& GetChildren() const { return children_; }

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
   * Removes a child from the tree without a host change, for a child whose backing is gone. A child
   * with open files moves to the detached list of the provider until its last close.
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
   * @return kAccessDenied on a read only mount, when the child or a descendant is open, or when the
   *         provider refuses.
   */
  [[nodiscard]] Result<void> DeleteChild(Node& child);

  /**
   * Deletes this node from its parent.
   *
   * @return kAccessDenied for the root or when the parent refuses.
   */
  [[nodiscard]] Result<void> Delete();

  /**
   * Moves the node under another directory of the same mount, with its descendants and open count.
   *
   * @param new_parent A directory of the same provider.
   * @param name The new last path component.
   * @param replace_existing True to replace a closed file with that name.
   * @return kAccessDenied on a read only mount, for the root, for a folder with an open file below
   *         it, or for an open or directory target, kInvalidPath when new_parent is the node or
   *         below it, kExists, or the provider error.
   */
  [[nodiscard]] Result<void> MoveTo(Node& new_parent, std::string_view name, bool replace_existing);

  /**
   * Cuts the file to 0 bytes.
   *
   * @return kUnsupported when the provider cannot truncate.
   */
  [[nodiscard]] virtual Result<void> Truncate();

  /** Reads the size again from the provider. */
  virtual void Refresh();

  /**
   * Reports whether the provider lost a cached child.
   *
   * @param child A child of this node.
   * @return True when the backing file of child is gone.
   */
  [[nodiscard]] virtual bool HasChildVanished(const Node& child) const;

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
   * @param new_parent The directory the node moves under.
   * @param name The new last path component.
   * @param replace_existing True when a backing with that name may be replaced.
   * @return Ok, kExists, or kUnsupported.
   */
  [[nodiscard]] virtual Result<void> MoveImpl(Node& new_parent, std::string_view name,
                                              bool replace_existing);

  /** Updates provider state that follows the path, after the node or an ancestor moved. */
  virtual void OnMoved();

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
  u32 open_count_ = 0;
  bool delete_on_close_ = false;
};

}  // namespace rex::filesystem
