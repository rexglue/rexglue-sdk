#include "filesystem/node.h"

#include <algorithm>
#include <format>
#include <mutex>

#include <rex/types.h>
#include <rex/filesystem/error.h>
#include <rex/logging/log.h>
#include <rex/result.h>
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
  return Err<size_t>(BuildError(FileSystemError::kAccessDenied, "the provider has no writes"));
}

Result<void> NodeStream::SetLength(u64 length) {
  (void)length;
  return Err(BuildError(FileSystemError::kUnsupported, "the provider has no size change"));
}

Result<void> NodeStream::Flush() {
  return Ok();
}

Result<std::span<const u8>> NodeStream::View() {
  return Err<std::span<const u8>>(
      BuildError(FileSystemError::kUnsupported, "the provider has no view"));
}

Node::Node(Provider& provider, Node* parent, std::string path)
    : path_(std::move(path)), provider_(&provider), parent_(parent) {
  absolute_path_ = utf8_join_guest_paths(provider.GetMountPath(), path_);
  name_ = std::string(utf8_find_name_from_guest_path(path_));
}

Node::~Node() = default;

bool Node::IsReadOnly() const {
  return provider_->IsReadOnly();
}

bool Node::DeleteOnClose() const {
  std::lock_guard lock(provider_->GetMutex());
  return delete_on_close_;
}

void Node::SetDeleteOnClose(bool delete_on_close) {
  std::lock_guard lock(provider_->GetMutex());
  delete_on_close_ = delete_on_close;
}

FileInfo Node::GetInfo() const {
  std::lock_guard lock(provider_->GetMutex());
  return {name_,        path_,        absolute_path_, attributes_, size_, allocation_size_,
          create_time_, access_time_, write_time_};
}

Node* Node::GetChild(std::string_view name) {
  std::lock_guard lock(provider_->GetMutex());
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
  std::lock_guard lock(provider_->GetMutex());
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
  std::lock_guard lock(provider_->GetMutex());
  return children_.emplace_back(std::move(child)).get();
}

void Node::DropChild(Node& child) {
  std::lock_guard lock(provider_->GetMutex());
  const auto it = std::ranges::find_if(
      children_, [&](const std::unique_ptr<Node>& entry) { return entry.get() == &child; });
  if (it == children_.end()) {
    return;
  }
  std::unique_ptr<Node> dropped = std::move(*it);
  children_.erase(it);
  if (dropped->open_count_ == 0) {
    return;
  }
  for (Node* node = this; node; node = node->parent_) {
    node->open_count_ -= dropped->open_count_;
  }
  dropped->parent_ = nullptr;
  provider_->Detach(std::move(dropped));
}

void Node::AddOpen() {
  std::lock_guard lock(provider_->GetMutex());
  for (Node* node = this; node; node = node->parent_) {
    ++node->open_count_;
  }
}

void Node::ReleaseOpen() {
  Provider& provider = *provider_;
  std::lock_guard lock(provider.GetMutex());
  for (Node* node = this; node; node = node->parent_) {
    --node->open_count_;
  }
  if (open_count_ == 0 && delete_on_close_ && parent_) {
    if (auto deleted = parent_->DeleteChild(*this); !deleted) {
      REXLOG_WARN(filesystem, "delete on close of '{}' failed, {}", absolute_path_,
                  deleted.error().message);
    }
  }
  provider.ReleaseDetached();
}

Result<Node*> Node::CreateChild(std::string_view name, FileAttributes attributes) {
  std::lock_guard lock(provider_->GetMutex());
  if (IsReadOnly()) {
    return Err<Node*>(BuildError(FileSystemError::kAccessDenied,
                                 std::format("'{}' is on a read only mount", absolute_path_)));
  }
  if (GetChild(name)) {
    return Err<Node*>(
        BuildError(FileSystemError::kExists, std::format("'{}\\{}' exists", absolute_path_, name)));
  }
  REX_TRY(std::unique_ptr<Node> child, CreateChildImpl(name, attributes));
  return children_.emplace_back(std::move(child)).get();
}

Result<void> Node::DeleteChild(Node& child) {
  std::lock_guard lock(provider_->GetMutex());
  if (IsReadOnly()) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("'{}' is on a read only mount", absolute_path_)));
  }
  if (child.parent_ != this) {
    return Err(BuildError(FileSystemError::kNotFound,
                          std::format("'{}' is not a child of '{}'", child.name_, absolute_path_)));
  }
  if (child.open_count_ != 0) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("'{}' is open", child.absolute_path_)));
  }
  REX_TRY_VOID(DeleteChildImpl(child));
  DropChild(child);
  return Ok();
}

Result<void> Node::Delete() {
  if (!parent_) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("the root of '{}' cannot be deleted", absolute_path_)));
  }
  return parent_->DeleteChild(*this);
}

Result<void> Node::MoveTo(Node& new_parent, std::string_view name, bool replace_existing) {
  std::lock_guard lock(provider_->GetMutex());
  if (IsReadOnly()) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("'{}' is on a read only mount", absolute_path_)));
  }
  if (!parent_) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("the root of '{}' cannot be moved", absolute_path_)));
  }
  for (const Node* node = &new_parent; node; node = node->parent_) {
    if (node == this) {
      return Err(BuildError(FileSystemError::kInvalidPath,
                            std::format("'{}' cannot move below itself", absolute_path_)));
    }
  }
  if (std::ranges::any_of(
          children_, [](const std::unique_ptr<Node>& child) { return child->open_count_ != 0; })) {
    return Err(BuildError(FileSystemError::kAccessDenied,
                          std::format("'{}' has an open file below it", absolute_path_)));
  }
  Node* existing = provider_->Resolve(utf8_join_guest_paths(new_parent.path_, name));
  if (existing == this) {
    existing = nullptr;
  }
  if (existing) {
    if (!replace_existing) {
      return Err(BuildError(FileSystemError::kExists,
                            std::format("'{}' exists", existing->absolute_path_)));
    }
    if (existing->IsDirectory() || existing->open_count_ != 0) {
      return Err(BuildError(FileSystemError::kAccessDenied,
                            std::format("'{}' cannot be replaced", existing->absolute_path_)));
    }
  }
  REX_TRY_VOID(MoveImpl(new_parent, name, existing != nullptr));
  if (existing) {
    new_parent.DropChild(*existing);
  }

  auto& siblings = parent_->children_;
  const auto it = std::ranges::find_if(
      siblings, [&](const std::unique_ptr<Node>& entry) { return entry.get() == this; });
  std::unique_ptr<Node> self = std::move(*it);
  siblings.erase(it);
  for (Node* node = parent_; node; node = node->parent_) {
    node->open_count_ -= open_count_;
  }
  parent_ = &new_parent;
  for (Node* node = parent_; node; node = node->parent_) {
    node->open_count_ += open_count_;
  }
  new_parent.children_.push_back(std::move(self));
  name_ = std::string(name);

  std::vector<Node*> pending{this};
  while (!pending.empty()) {
    Node* node = pending.back();
    pending.pop_back();
    node->path_ = utf8_join_guest_paths(node->parent_->path_, node->name_);
    node->absolute_path_ = utf8_join_guest_paths(provider_->GetMountPath(), node->path_);
    node->OnMoved();
    for (const std::unique_ptr<Node>& child : node->children_) {
      pending.push_back(child.get());
    }
  }
  return Ok();
}

Result<void> Node::Truncate() {
  return Err(BuildError(FileSystemError::kUnsupported, "the provider cannot truncate"));
}

void Node::Refresh() {}

bool Node::HasChildVanished(const Node& child) const {
  (void)child;
  return false;
}

Result<void> Node::SetAttributes(FileAttributes attributes) {
  (void)attributes;
  return Err(BuildError(FileSystemError::kUnsupported, "the provider has no attribute change"));
}

Result<void> Node::SetTimes(const FileTimes& times) {
  (void)times;
  return Err(BuildError(FileSystemError::kUnsupported, "the provider has no time change"));
}

Result<std::unique_ptr<Node>> Node::CreateChildImpl(std::string_view name,
                                                    FileAttributes attributes) {
  (void)name;
  (void)attributes;
  return Err<std::unique_ptr<Node>>(
      BuildError(FileSystemError::kUnsupported, "the provider cannot create"));
}

Result<void> Node::DeleteChildImpl(Node& child) {
  (void)child;
  return Err(BuildError(FileSystemError::kUnsupported, "the provider cannot delete"));
}

Result<void> Node::MoveImpl(Node& new_parent, std::string_view name, bool replace_existing) {
  (void)new_parent;
  (void)name;
  (void)replace_existing;
  return Err(BuildError(FileSystemError::kUnsupported, "the provider cannot rename"));
}

void Node::OnMoved() {}

}  // namespace rex::filesystem
