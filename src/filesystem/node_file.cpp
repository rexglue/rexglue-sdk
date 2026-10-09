#include "filesystem/node_file.h"

#include <format>

#include <rex/filesystem/error.h>

#include "filesystem/provider.h"

namespace rex::filesystem {

NodeFile::NodeFile(Node& node, Access access, std::unique_ptr<NodeStream> stream)
    : provider_(node.provider().shared_from_this()),
      node_(&node),
      access_(access),
      stream_(std::move(stream)) {
  node_->AddOpen();
}

NodeFile::~NodeFile() {
  stream_.reset();
  node_->ReleaseOpen();
}

FileInfo NodeFile::Info() const {
  return node_->Info();
}

void NodeFile::Refresh() {
  node_->Refresh();
}

VolumeInfo NodeFile::Volume() const {
  return node_->provider().Volume();
}

Result<size_t> NodeFile::Read(std::span<u8> buffer, u64 offset) {
  return stream_->Read(buffer, offset);
}

Result<size_t> NodeFile::Write(std::span<const u8> buffer, u64 offset) {
  return stream_->Write(buffer, offset);
}

Result<void> NodeFile::SetLength(u64 length) {
  return stream_->SetLength(length);
}

Result<void> NodeFile::Flush() {
  return stream_->Flush();
}

Result<void> NodeFile::SetAttributes(FileAttributes attributes) {
  return node_->SetAttributes(attributes);
}

Result<void> NodeFile::SetTimes(const FileTimes& times) {
  return node_->SetTimes(times);
}

void NodeFile::SetDeleteOnClose(bool delete_on_close) {
  node_->set_delete_on_close(delete_on_close);
}

Result<std::unique_ptr<IDirectory>> NodeFile::List(std::string_view pattern) {
  if (!node_->is_directory()) {
    return Err<std::unique_ptr<IDirectory>>(MakeError(
        Error::kNotDirectory, std::format("'{}' is not a directory", node_->absolute_path())));
  }
  return std::unique_ptr<IDirectory>(std::make_unique<NodeDirectory>(*node_, pattern));
}

Result<std::span<const u8>> NodeFile::View() {
  return stream_->View();
}

NodeDirectory::NodeDirectory(Node& node, std::string_view pattern)
    : provider_(node.provider().shared_from_this()), node_(&node), matcher_(pattern) {
  node_->AddOpen();
}

NodeDirectory::~NodeDirectory() {
  node_->ReleaseOpen();
}

std::optional<FileInfo> NodeDirectory::Next() {
  Node* child = node_->IterateChildren(matcher_, &index_);
  if (!child) {
    return std::nullopt;
  }
  return child->Info();
}

}  // namespace rex::filesystem
