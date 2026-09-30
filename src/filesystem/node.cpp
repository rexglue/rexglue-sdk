/**
 * @file        filesystem/node.cpp
 * @brief       The shared tree node of a mount and the byte stream of 1 open node
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/vfs/entry.cc (Ben Vanik, 2020).
 */

#include "filesystem/node.h"

#include <algorithm>
#include <format>
#include <mutex>

#include <rex/filesystem/error.h>
#include <rex/macros.h>
#include <rex/string.h>

#include "filesystem/provider.h"
#include "filesystem/wildcard.h"

namespace rex::filesystem {

bool CanRead(Access access) {
  return (access & (Access::kGenericRead | Access::kReadData)) != Access::kNone;
}

bool CanWrite(Access access) {
  return (access & (Access::kGenericWrite | Access::kWriteData | Access::kAppendData)) !=
         Access::kNone;
}

bool CanSetLength(Access access) {
  return (access & (Access::kGenericWrite | Access::kWriteData)) != Access::kNone;
}

Result<size_t> NodeStream::Write(std::span<const u8> buffer, u64 offset) {
  (void)buffer;
  (void)offset;
  return Err<size_t>(MakeError(Error::kAccessDenied, "the provider has no writes"));
}

Result<void> NodeStream::SetLength(u64 length) {
  (void)length;
  return Err(MakeError(Error::kUnsupported, "the provider has no size change"));
}

Result<std::span<const u8>> NodeStream::View() {
  return Err<std::span<const u8>>(MakeError(Error::kUnsupported, "the provider has no view"));
}

Node::Node(Provider& provider, Node* parent, std::string path)
    : path_(std::move(path)), provider_(&provider), parent_(parent) {
  absolute_path_ = utf8_join_guest_paths(provider.mount_path(), path_);
  name_ = std::string(utf8_find_name_from_guest_path(path_));
}

Node::~Node() = default;

bool Node::read_only() const {
  return provider_->read_only();
}

FileInfo Node::Info() const {
  std::lock_guard lock(provider_->mutex());
  return {name_,        path_,        absolute_path_, attributes_, size_, allocation_size_,
          create_time_, access_time_, write_time_};
}

Node* Node::GetChild(std::string_view name) {
  std::lock_guard lock(provider_->mutex());
  const auto it = std::ranges::find_if(children_, [&](const std::unique_ptr<Node>& child) {
    return child->name_.size() == name.size() && utf8_equal_case(child->name_, name);
  });
  return it == children_.end() ? nullptr : it->get();
}

Node* Node::ResolvePath(std::string_view path) {
  Node* node = this;
  for (std::string_view part : utf8_split_path(path)) {
    node = node->GetChild(part);
    if (!node) {
      return nullptr;
    }
  }
  return node;
}

Node* Node::IterateChildren(const WildcardMatcher& matcher, size_t* index) {
  std::lock_guard lock(provider_->mutex());
  while (*index < children_.size()) {
    Node* child = children_[*index].get();
    ++*index;
    if (matcher.Match(child->name_)) {
      return child;
    }
  }
  return nullptr;
}

Node* Node::AddChild(std::unique_ptr<Node> child) {
  std::lock_guard lock(provider_->mutex());
  return children_.emplace_back(std::move(child)).get();
}

void Node::DropChild(Node& child) {
  std::lock_guard lock(provider_->mutex());
  std::erase_if(children_,
                [&](const std::unique_ptr<Node>& entry) { return entry.get() == &child; });
}

Result<Node*> Node::CreateChild(std::string_view name, FileAttributes attributes) {
  std::lock_guard lock(provider_->mutex());
  if (read_only()) {
    return Err<Node*>(MakeError(Error::kAccessDenied,
                                std::format("'{}' is on a read only mount", absolute_path_)));
  }
  if (GetChild(name)) {
    return Err<Node*>(
        MakeError(Error::kExists, std::format("'{}\\{}' exists", absolute_path_, name)));
  }
  REX_TRY(std::unique_ptr<Node> child, CreateChildImpl(name, attributes));
  return children_.emplace_back(std::move(child)).get();
}

Result<void> Node::DeleteChild(Node& child) {
  std::lock_guard lock(provider_->mutex());
  if (read_only()) {
    return Err(MakeError(Error::kAccessDenied,
                         std::format("'{}' is on a read only mount", absolute_path_)));
  }
  if (child.parent_ != this) {
    return Err(MakeError(Error::kNotFound,
                         std::format("'{}' is not a child of '{}'", child.name_, absolute_path_)));
  }
  REX_TRY_VOID(DeleteChildImpl(child));
  DropChild(child);
  return Ok();
}

Result<void> Node::Delete() {
  if (!parent_) {
    return Err(MakeError(Error::kAccessDenied,
                         std::format("the root of '{}' cannot be deleted", absolute_path_)));
  }
  return parent_->DeleteChild(*this);
}

Result<void> Node::Rename(std::string_view path) {
  const std::string text(path);
  std::vector<std::string_view> parts = utf8_split_path(text);
  if (!parts.empty()) {
    parts.erase(parts.begin());
  }
  REX_TRY_VOID(RenameImpl(parts));
  std::lock_guard lock(provider_->mutex());
  path_ = utf8_join_guest_paths(parts);
  absolute_path_ = utf8_join_guest_paths(provider_->mount_path(), path_);
  name_ = std::string(utf8_find_name_from_guest_path(text));
  return Ok();
}

Result<void> Node::Truncate() {
  return Err(MakeError(Error::kUnsupported, "the provider cannot truncate"));
}

void Node::Refresh() {}

bool Node::ChildVanished(const Node& child) const {
  (void)child;
  return false;
}

Result<void> Node::SetAttributes(FileAttributes attributes) {
  (void)attributes;
  return Err(MakeError(Error::kUnsupported, "the provider has no attribute change"));
}

Result<void> Node::SetTimes(const FileTimes& times) {
  (void)times;
  return Err(MakeError(Error::kUnsupported, "the provider has no time change"));
}

Result<std::unique_ptr<Node>> Node::CreateChildImpl(std::string_view name,
                                                    FileAttributes attributes) {
  (void)name;
  (void)attributes;
  return Err<std::unique_ptr<Node>>(MakeError(Error::kUnsupported, "the provider cannot create"));
}

Result<void> Node::DeleteChildImpl(Node& child) {
  (void)child;
  return Err(MakeError(Error::kUnsupported, "the provider cannot delete"));
}

Result<void> Node::RenameImpl(std::span<const std::string_view> parts) {
  (void)parts;
  return Err(MakeError(Error::kUnsupported, "the provider cannot rename"));
}

}  // namespace rex::filesystem
