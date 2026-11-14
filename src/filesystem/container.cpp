#include "filesystem/container.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <ranges>
#include <system_error>

#include <rex/logging/log_category.h>
#include <rex/filesystem/file_system_error.h>
#include <rex/filesystem/file_stat.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform/file.h>
#include <rex/string/text.h>

#include "container_parse.h"

namespace rex::filesystem {

namespace {

constexpr size_t kExtractChunk = size_t{4} * 1024 * 1024;

std::span<const u8> BytesOf(const memory::MappedMemory& map) {
  return {map.GetData(), map.GetSize()};
}

bool IsPackageMagic(std::span<const u8> bytes) {
  auto magic = detail::ReadAt<BE<XContentPackageType>>(bytes, 0);
  if (!magic) {
    return false;
  }
  const XContentPackageType value = *magic;
  return value == XContentPackageType::kCon || value == XContentPackageType::kLive ||
         value == XContentPackageType::kPirs;
}

std::optional<std::filesystem::path> FindPackageIn(const std::filesystem::path& folder) {
  std::deque<std::filesystem::path> queue{folder};
  std::error_code ec;
  while (!queue.empty()) {
    const std::filesystem::path current = queue.front();
    queue.pop_front();
    for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
      if (entry.is_directory(ec)) {
        queue.push_back(entry.path());
        continue;
      }
      auto map = memory::MappedMemory::Open(entry.path(), memory::MappedMemory::Mode::kRead, 0, 4);
      if (map && IsPackageMagic(BytesOf(*map))) {
        return entry.path();
      }
    }
  }
  return std::nullopt;
}

bool IsSafeName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find_first_of("\\/:") == std::string_view::npos;
}

}  // namespace

Container::Container() = default;
Container::Container(Container&& other) noexcept = default;
Container& Container::operator=(Container&& other) noexcept = default;
Container::~Container() = default;

Result<Container> Container::Open(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path file = path;
  if (std::filesystem::is_directory(path, ec)) {
    auto found = FindPackageIn(path);
    if (!found) {
      return Err<Container>(
          BuildError(FileSystemError::kNotFound,
                     std::format("no package found under '{}'", string::PathToUTF8(path))));
    }
    file = *found;
  }
  auto map = memory::MappedMemory::Open(file, memory::MappedMemory::Mode::kRead);
  if (!map) {
    return Err<Container>(BuildError(FileSystemError::kNotFound,
                                     std::format("cannot map '{}'", string::PathToUTF8(file))));
  }

  Container container;
  container.path_ = file;
  container.nodes_.emplace_back().info.attributes = FileAttributes::kDirectory;
  const std::span<const u8> bytes = BytesOf(*map);

  if (IsPackageMagic(bytes)) {
    REX_TRY(XContentPackageHeader header, ReadSTFSHeader(bytes));
    container.header_ = std::make_unique<XContentPackageHeader>(header);
    container.total_size_ = map->GetSize();
    const XContentPackageHeader& stored = *container.header_;
    const XContentVolumeType volume_type =
        memory::load<BE<XContentVolumeType>>(&stored.metadata.volume_type);
    const u32 data_file_count = memory::load<be_u32>(&stored.metadata.data_file_count);
    if (volume_type == XContentVolumeType::kSTFS) {
      container.type_ = ContainerType::kSTFS;
      container.files_.push_back(std::move(map));
      REX_TRY_VOID(detail::ParseSTFS(stored, BytesOf(*container.files_[0]), container.nodes_));
    } else if (volume_type == XContentVolumeType::kSVOD) {
      container.type_ = ContainerType::kSVOD;
      if (data_file_count <= 1) {
        container.files_.push_back(std::move(map));
      } else {
        const std::filesystem::path fragments = platform::ResolveEntryCase(
            file.parent_path(), string::PathToUTF8(file.filename()) + ".data");
        std::vector<std::filesystem::path> names;
        for (const auto& entry : std::filesystem::directory_iterator(fragments, ec)) {
          if (!entry.is_directory(ec)) {
            names.push_back(entry.path());
          }
        }
        std::ranges::sort(names, {}, [](const std::filesystem::path& p) { return p.filename(); });
        container.total_size_ = 0;
        if (names.size() != data_file_count) {
          return Err<Container>(
              BuildError(FileSystemError::kCorrupt,
                         std::format("SVOD lists {} data fragments, '{}' holds {}", data_file_count,
                                     string::PathToUTF8(fragments), names.size())));
        }
        for (const auto& name : names) {
          auto fragment = memory::MappedMemory::Open(name, memory::MappedMemory::Mode::kRead);
          if (!fragment) {
            return Err<Container>(
                BuildError(FileSystemError::kNotFound,
                           std::format("cannot map '{}'", string::PathToUTF8(name))));
          }
          container.total_size_ += fragment->GetSize();
          container.files_.push_back(std::move(fragment));
        }
      }
      std::vector<std::span<const u8>> spans;
      spans.reserve(container.files_.size());
      for (const auto& fragment : container.files_) {
        spans.push_back(BytesOf(*fragment));
      }
      REX_TRY_VOID(detail::ParseSVOD(stored, spans, container.nodes_));
    } else {
      return Err<Container>(BuildError(
          FileSystemError::kCorrupt,
          std::format("volume type {} is not STFS or SVOD", std::to_underlying(volume_type))));
    }
  } else if (detail::IsGDFX(bytes)) {
    container.type_ = ContainerType::kGDFX;
    container.total_size_ = map->GetSize();
    container.files_.push_back(std::move(map));
    REX_TRY_VOID(detail::ParseGDFX(BytesOf(*container.files_[0]), container.nodes_));
  } else {
    return Err<Container>(BuildError(
        FileSystemError::kCorrupt,
        std::format("'{}' is not an STFS, SVOD or GDFX image", string::PathToUTF8(file))));
  }
  return container;
}

const ContainerNode* Container::Find(std::string_view path) const {
  const ContainerNode* node = &nodes_.front();
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find_first_of("\\/", start);
    if (end == std::string_view::npos) {
      end = path.size();
    }
    const std::string_view part = path.substr(start, end - start);
    if (!part.empty()) {
      auto child = std::ranges::find_if(node->children, [&](u32 index) {
        return string::utf8_equal_case(nodes_[index].info.name, part);
      });
      if (child == node->children.end()) {
        return nullptr;
      }
      node = &nodes_[*child];
    }
    start = end + 1;
  }
  return node;
}

std::string Container::GetNodePath(const ContainerNode& node) const {
  std::vector<std::string_view> parts;
  const ContainerNode* current = &node;
  while (current != &nodes_.front()) {
    parts.push_back(current->info.name);
    current = &nodes_[current->parent];
  }
  std::string result;
  for (const auto part : parts | std::views::reverse) {
    if (!result.empty()) {
      result.push_back('\\');
    }
    result.append(part);
  }
  return result;
}

Result<size_t> Container::Read(const ContainerNode& node, u64 offset, std::span<u8> buffer) const {
  if (offset >= node.info.size) {
    return Err<size_t>(
        BuildError(FileSystemError::kEndOfFile, "read starts at or past the end of the file"));
  }
  u64 remaining = std::min<u64>(buffer.size(), node.info.size - offset);
  u64 source = 0;
  size_t copied = 0;
  for (const ContainerExtent& extent : node.extents) {
    if (remaining == 0) {
      break;
    }
    if (source + extent.length <= offset) {
      source += extent.length;
      continue;
    }
    const u64 skip = offset > source ? offset - source : 0;
    const u64 length = std::min(extent.length - skip, remaining);
    const std::span<const u8> file = BytesOf(*files_[extent.file]);
    const u64 start = extent.offset + skip;
    if (!detail::IsInBounds(file, start, length)) {
      REXFS_WARN("container read of '{}' stops at the end of the data file", node.info.name);
      break;
    }
    std::memcpy(buffer.data() + copied, file.data() + start, length);
    copied += length;
    remaining -= length;
    source += extent.length;
  }
  return copied;
}

std::span<const u8> Container::View(const ContainerNode& node) const {
  if (node.extents.size() != 1 || node.extents.front().length < node.info.size) {
    return {};
  }
  const ContainerExtent& extent = node.extents.front();
  const std::span<const u8> file = BytesOf(*files_[extent.file]);
  if (!detail::IsInBounds(file, extent.offset, node.info.size)) {
    return {};
  }
  return file.subspan(extent.offset, node.info.size);
}

Result<void> Container::Extract(const std::filesystem::path& destination) const {
  std::error_code ec;
  const bool existed = std::filesystem::exists(destination, ec);
  std::vector<std::filesystem::path> written;
  auto fail = [&](FileSystemError code, std::string message) -> Result<void> {
    if (!existed) {
      std::filesystem::remove_all(destination, ec);
    } else {
      for (const auto& path : written | std::views::reverse) {
        std::filesystem::remove(path, ec);
      }
    }
    return Err(BuildError(code, std::move(message)));
  };
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    return fail(FileSystemError::kIO,
                std::format("cannot create '{}'", string::PathToUTF8(destination)));
  }
  std::vector<std::filesystem::path> host(nodes_.size());
  host[0] = destination;
  std::vector<u8> chunk;
  for (size_t index = 1; index < nodes_.size(); ++index) {
    const ContainerNode& node = nodes_[index];
    if (!IsSafeName(node.info.name)) {
      return fail(FileSystemError::kInvalidPath,
                  std::format("container name '{}' is refused", node.info.name));
    }
    host[index] = host[node.parent] / string::to_path(node.info.name);
    const bool fresh = !std::filesystem::exists(host[index], ec);
    if (HasFlags(node.info.attributes, FileAttributes::kDirectory)) {
      std::filesystem::create_directories(host[index], ec);
      if (ec) {
        return fail(FileSystemError::kIO,
                    std::format("cannot create '{}'", string::PathToUTF8(host[index])));
      }
      if (fresh) {
        written.push_back(host[index]);
      }
      continue;
    }
    std::ofstream stream(host[index], std::ios::binary | std::ios::trunc);
    if (!stream) {
      return fail(FileSystemError::kIO,
                  std::format("cannot open '{}'", string::PathToUTF8(host[index])));
    }
    if (fresh) {
      written.push_back(host[index]);
    }
    chunk.resize(static_cast<size_t>(std::min<u64>(node.info.size, kExtractChunk)));
    for (u64 offset = 0; offset < node.info.size;) {
      auto read = Read(node, offset, chunk);
      if (!read || *read == 0) {
        return fail(FileSystemError::kCorrupt, std::format("cannot read '{}'", GetNodePath(node)));
      }
      if (!stream.write(reinterpret_cast<const char*>(chunk.data()),
                        static_cast<std::streamsize>(*read))) {
        return fail(FileSystemError::kIO,
                    std::format("cannot write '{}'", string::PathToUTF8(host[index])));
      }
      offset += *read;
    }
    stream.close();
    if (!stream) {
      return fail(FileSystemError::kIO,
                  std::format("cannot flush '{}'", string::PathToUTF8(host[index])));
    }
  }
  return Ok();
}

Result<XContentPackageHeader> ReadSTFSHeader(std::span<const u8> bytes) {
  if (bytes.size() < sizeof(XContentPackageHeader)) {
    return Err<XContentPackageHeader>(BuildError(
        FileSystemError::kCorrupt, std::format("package header needs {:#x} bytes, {:#x} present",
                                               sizeof(XContentPackageHeader), bytes.size())));
  }
  XContentPackageHeader header;
  std::memcpy(&header, bytes.data(), sizeof(XContentPackageHeader));
  if (!header.header.IsMagicValid()) {
    return Err<XContentPackageHeader>(
        BuildError(FileSystemError::kCorrupt, "package magic is not CON, LIVE or PIRS"));
  }
  return header;
}

Result<XContentPackageHeader> ReadSTFSHeader(const std::filesystem::path& path) {
  auto map = memory::MappedMemory::Open(path, memory::MappedMemory::Mode::kRead);
  if (!map) {
    return Err<XContentPackageHeader>(BuildError(
        FileSystemError::kNotFound, std::format("cannot open '{}'", string::PathToUTF8(path))));
  }
  return ReadSTFSHeader(std::span<const u8>(map->GetData(), map->GetSize()));
}

}  // namespace rex::filesystem
