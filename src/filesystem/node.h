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

#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

class Provider;
class WildcardMatcher;

/**
 * Reports whether access allows reading.
 *
 * @return True when a generic read or read data bit is set.
 */
[[nodiscard]] bool CanRead(Access access);

/**
 * Reports whether access allows writing.
 *
 * @return True when a generic write, write data or append data bit is set.
 */
[[nodiscard]] bool CanWrite(Access access);

/**
 * Reports whether access allows a size change.
 *
 * @return True when a generic write or write data bit is set.
 */
[[nodiscard]] bool CanSetLength(Access access);

/** Byte access of an open node, served by its provider. */
class NodeStream {
 public:
  virtual ~NodeStream() = default;

  /**
   * @param buffer Destination.
   * @return The bytes copied, or the error.
   */
  [[nodiscard]] virtual Result<size_t> Read(std::span<u8> buffer, u64 offset) = 0;

  /**
   * @param buffer Source bytes.
   * @return The bytes written, or kAccessDenied when the provider has no writes.
   */
  [[nodiscard]] virtual Result<size_t> Write(std::span<const u8> buffer, u64 offset);

  /**
   * @return kUnsupported when the provider has no size change.
   */
  [[nodiscard]] virtual Result<void> SetLength(u64 length);

  /**
   * Writes buffered data of the node to its device.
   *
   * @return Ok when the provider has nothing to write, or the error.
   */
  [[nodiscard]] virtual Result<void> Flush();

  /** Returns the whole node as read only bytes, or kUnsupported. */
  [[nodiscard]] virtual Result<std::span<const u8>> View();
};

/**
 * A file or directory of a mount. Every open handle on the path shares the node. Tree changes take
 * the provider mutex.
 */
class Node : public NonCopyable {
 public:
  virtual ~Node();

  [[nodiscard]] Provider& GetProvider() const { return *provider_; }

  /** Returns the parent directory, null for the root. */
  [[nodiscard]] Node* GetParent() const { return parent_; }

  /** Returns the last path component, empty for the root. */
  [[nodiscard]] const std::string& GetName() const { return info_.name; }

  /** Returns the path relative to the mount. */
  [[nodiscard]] const std::string& GetPath() const { return info_.path; }

  /** Returns the guest path including the mount path. */
  [[nodiscard]] const std::string& GetAbsolutePath() const { return info_.absolute_path; }

  [[nodiscard]] bool IsDirectory() const {
    return HasFlags(info_.attributes, FileAttributes::kDirectory);
  }

  /** Reports whether the mount of the node refuses writes. */
  [[nodiscard]] bool IsReadOnly() const;

  /** Sets or withdraws a delete on close request. */
  void SetDeleteOnClose(bool delete_on_close);

  /** Returns the count of open files and walks on the node and its descendants. */
  [[nodiscard]] u32 GetOpenCount() const { return open_count_; }

  /** Counts one open file or walk on the node and its ancestors. */
  void AddOpen();

  /**
   * Removes one open file or walk from the count. At the last close the node is deleted when delete
   * on close is set, and a detached node is freed. The node may be gone when this returns.
   */
  void ReleaseOpen();

  [[nodiscard]] FileStat GetInfo() const;

  /** Returns the children in insertion order. */
  [[nodiscard]] const std::vector<std::unique_ptr<Node>>& GetChildren() const { return children_; }

  /** Returns the child with a name, case insensitive, or null. */
  [[nodiscard]] Node* GetChild(std::string_view name);

  /**
   * Walks a relative path one component at a time.
   *
   * @param path Components split by a backslash, empty for this node.
   * @return The node, or null when a component is missing.
   */
  [[nodiscard]] Node* ResolvePath(std::string_view path);

  /**
   * Returns the next child at or after index that matches, or null when none is left.
   *
   * @param index The child index to start at, advanced past the returned child.
   */
  [[nodiscard]] Node* IterateChildren(const WildcardMatcher& matcher, size_t* index);

  /** Adds a child a provider built. No host change is made. */
  Node* AddChild(std::unique_ptr<Node> child);

  /**
   * Removes a child from the tree without a host change, for a child whose backing is gone. A child
   * with open files moves to the detached list of the provider until its last close.
   */
  void RemoveChild(Node& child);

  /**
   * Creates a child through the provider.
   *
   * @param attributes kDirectory for a directory, kNormal for a file.
   * @return The child, kAccessDenied on a read only mount or when the provider refuses, kExists.
   */
  [[nodiscard]] Result<Node*> CreateChild(std::string_view name, FileAttributes attributes);

  /**
   * Deletes a child through the provider.
   *
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
   * Cuts the file to zero bytes.
   *
   * @return kUnsupported when the provider cannot truncate.
   */
  [[nodiscard]] virtual Result<void> Truncate();

  /** Reads the size again from the provider. */
  virtual void Refresh();

  /**
   * Reports whether the provider lost a cached child.
   *
   * @return True when the backing file of child is gone.
   */
  [[nodiscard]] virtual bool HasChildVanished(const Node& child) const;

  /**
   * @return The stream, or the provider error.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<NodeStream>> Open(Access access) = 0;

  /**
   * Replaces the attribute bits.
   *
   * @return kUnsupported when the provider has no attribute change.
   */
  [[nodiscard]] virtual Result<void> SetAttributes(FileAttributes attributes);

  /**
   * Changes the times. An empty field keeps its time.
   *
   * @return kUnsupported when the provider has no time change.
   */
  [[nodiscard]] virtual Result<void> SetTimes(const FileTimes& times);

 protected:
  /**
   * @param provider The provider that owns the node.
   * @param parent The parent, or null for the root.
   */
  Node(Provider& provider, Node* parent, std::string path);

  /**
   * Creates the backing of a child.
   *
   * @param attributes kDirectory for a directory, kNormal for a file.
   * @return The child, or kUnsupported.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<Node>> CreateChildBacking(std::string_view name,
                                                                         FileAttributes attributes);

  /**
   * Deletes the backing of a child.
   *
   * @return Ok, or kUnsupported.
   */
  [[nodiscard]] virtual Result<void> DeleteChildBacking(Node& child);

  /**
   * Moves the backing of this node.
   *
   * @param name The new last path component.
   * @param replace_existing True when a backing with that name may be replaced.
   * @return Ok, kExists, or kUnsupported.
   */
  [[nodiscard]] virtual Result<void> MoveBacking(Node& new_parent, std::string_view name,
                                                 bool replace_existing);

  /** Updates provider state that follows the path, after the node or an ancestor moved. */
  virtual void OnMoved();

  FileStat info_;                               /**< Name, paths, attributes, sizes and times. */
  std::vector<std::unique_ptr<Node>> children_; /**< Children in insertion order. */

 private:
  Provider* provider_;
  Node* parent_;
  u32 open_count_ = 0;
  bool delete_on_close_ = false;
};

}  // namespace rex::filesystem
