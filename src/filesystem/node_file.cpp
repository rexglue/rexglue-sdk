#include "filesystem/node_file.h"

#include <format>

#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/file_stat.h>
#include <rex/filesystem/file_open.h>
#include <rex/filesystem/directory.h>

#include "filesystem/provider.h"

namespace rex::filesystem {

NodeFile::NodeFile(Node& node, Access access, std::unique_ptr<NodeStream> stream)
    : provider_(node.GetProvider().shared_from_this()),
      node_(&node),
      access_(access),
      stream_(std::move(stream)) {
  node_->AddOpen();
}

NodeFile::~NodeFile() {
  stream_.reset();
  node_->ReleaseOpen();
}

FileStat NodeFile::GetInfo() const {
  return node_->GetInfo();
}

void NodeFile::Refresh() {
  node_->Refresh();
}

VolumeStat NodeFile::GetVolume() const {
  return node_->GetProvider().GetVolume();
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
  node_->SetDeleteOnClose(delete_on_close);
}

Result<std::unique_ptr<IDirectory>> NodeFile::List(std::string_view pattern) {
  if (!node_->IsDirectory()) {
    return Err<std::unique_ptr<IDirectory>>(
        BuildError(FileSystemError::kNotDirectory,
                   std::format("'{}' is not a directory", node_->GetAbsolutePath())));
  }
  return std::unique_ptr<IDirectory>(std::make_unique<NodeDirectory>(*node_, pattern));
}

Result<std::span<const u8>> NodeFile::View() {
  return stream_->View();
}

NodeDirectory::NodeDirectory(Node& node, std::string_view pattern)
    : provider_(node.GetProvider().shared_from_this()), node_(&node), matcher_(pattern) {
  node_->AddOpen();
}

NodeDirectory::~NodeDirectory() {
  node_->ReleaseOpen();
}

std::optional<FileStat> NodeDirectory::Next() {
  Node* child = node_->IterateChildren(matcher_, &index_);
  if (!child) {
    return std::nullopt;
  }
  return child->GetInfo();
}

}  // namespace rex::filesystem
