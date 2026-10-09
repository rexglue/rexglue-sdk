#include <rex/filesystem/container.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <ranges>
#include <system_error>

#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>
#include <rex/string.h>

#include "container_parse.h"

namespace rex::filesystem {

namespace {

constexpr size_t kExtractChunk = size_t{4} * 1024 * 1024;

std::span<const u8> BytesOf(const memory::MappedMemory& map) {
  return {map.data(), map.size()};
}

bool IsPackageMagic(std::span<const u8> bytes) {
  auto magic = detail::ReadAt<be<XContentPackageType>>(bytes, 0);
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
      return Err<Container>(MakeError(
          Error::kNotFound, std::format("no package found under '{}'", path_to_utf8(path))));
    }
    file = *found;
  }
  auto map = memory::MappedMemory::Open(file, memory::MappedMemory::Mode::kRead);
  if (!map) {
    return Err<Container>(
        MakeError(Error::kNotFound, std::format("cannot map '{}'", path_to_utf8(file))));
  }

  Container container;
  container.path_ = file;
  container.nodes_.emplace_back().attributes = FileAttributes::kDirectory;
  const std::span<const u8> bytes = BytesOf(*map);

  if (IsPackageMagic(bytes)) {
    REX_TRY(STFSHeader header, ReadSTFSHeader(bytes));
    container.header_ = std::make_unique<STFSHeader>(header);
    container.total_size_ = map->size();
    const STFSHeader& stored = *container.header_;
    if (stored.metadata.volume_type == XContentVolumeType::kSTFS) {
      container.kind_ = ContainerKind::kSTFS;
      container.files_.push_back(std::move(map));
      REX_TRY_VOID(detail::ParseSTFS(stored, BytesOf(*container.files_[0]), container.nodes_));
    } else if (stored.metadata.volume_type == XContentVolumeType::kSVOD) {
      container.kind_ = ContainerKind::kSVOD;
      if (stored.metadata.data_file_count <= 1) {
        container.files_.push_back(std::move(map));
      } else {
        std::filesystem::path fragments = file;
        fragments += ".data";
        std::vector<std::filesystem::path> names;
        for (const auto& entry : std::filesystem::directory_iterator(fragments, ec)) {
          if (!entry.is_directory(ec)) {
            names.push_back(entry.path());
          }
        }
        std::ranges::sort(names, {}, [](const std::filesystem::path& p) { return p.filename(); });
        container.total_size_ = 0;
        if (names.size() != stored.metadata.data_file_count) {
          return Err<Container>(MakeError(
              Error::kCorrupt, std::format("SVOD lists {} data fragments, '{}' holds {}",
                                           static_cast<u32>(stored.metadata.data_file_count),
                                           path_to_utf8(fragments), names.size())));
        }
        for (const auto& name : names) {
          auto fragment = memory::MappedMemory::Open(name, memory::MappedMemory::Mode::kRead);
          if (!fragment) {
            return Err<Container>(
                MakeError(Error::kNotFound, std::format("cannot map '{}'", path_to_utf8(name))));
          }
          container.total_size_ += fragment->size();
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
      return Err<Container>(
          MakeError(Error::kCorrupt, std::format("volume type {} is not STFS or SVOD",
                                                 std::to_underlying(static_cast<XContentVolumeType>(
                                                     stored.metadata.volume_type)))));
    }
  } else if (detail::IsGDFX(bytes)) {
    container.kind_ = ContainerKind::kGDFX;
    container.total_size_ = map->size();
    container.files_.push_back(std::move(map));
    REX_TRY_VOID(detail::ParseGDFX(BytesOf(*container.files_[0]), container.nodes_));
  } else {
    return Err<Container>(
        MakeError(Error::kCorrupt,
                  std::format("'{}' is not an STFS, SVOD or GDFX image", path_to_utf8(file))));
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
      auto child = std::ranges::find_if(
          node->children, [&](u32 index) { return utf8_equal_case(nodes_[index].name, part); });
      if (child == node->children.end()) {
        return nullptr;
      }
      node = &nodes_[*child];
    }
    start = end + 1;
  }
  return node;
}

std::string Container::PathOf(const ContainerNode& node) const {
  std::vector<std::string_view> parts;
  const ContainerNode* current = &node;
  while (current != &nodes_.front()) {
    parts.push_back(current->name);
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
  if (offset >= node.size) {
    return Err<size_t>(MakeError(Error::kEndOfFile, "read starts at or past the end of the file"));
  }
  u64 remaining = std::min<u64>(buffer.size(), node.size - offset);
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
    if (!detail::InBounds(file, start, length)) {
      REXLOG_WARN(filesystem, "container read of '{}' stops at the end of the data file",
                  node.name);
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
  if (node.extents.size() != 1 || node.extents.front().length < node.size) {
    return {};
  }
  const ContainerExtent& extent = node.extents.front();
  const std::span<const u8> file = BytesOf(*files_[extent.file]);
  if (!detail::InBounds(file, extent.offset, node.size)) {
    return {};
  }
  return file.subspan(extent.offset, node.size);
}

Result<void> Container::Extract(const std::filesystem::path& destination) const {
  std::error_code ec;
  const bool existed = std::filesystem::exists(destination, ec);
  std::vector<std::filesystem::path> written;
  auto fail = [&](Error code, std::string message) -> Result<void> {
    if (!existed) {
      std::filesystem::remove_all(destination, ec);
    } else {
      for (const auto& path : written | std::views::reverse) {
        std::filesystem::remove(path, ec);
      }
    }
    return Err(MakeError(code, std::move(message)));
  };
  std::filesystem::create_directories(destination, ec);
  if (ec) {
    return fail(Error::kIO, std::format("cannot create '{}'", path_to_utf8(destination)));
  }
  std::vector<std::filesystem::path> host(nodes_.size());
  host[0] = destination;
  std::vector<u8> chunk;
  for (size_t index = 1; index < nodes_.size(); ++index) {
    const ContainerNode& node = nodes_[index];
    if (!IsSafeName(node.name)) {
      return fail(Error::kInvalidPath, std::format("container name '{}' is refused", node.name));
    }
    host[index] = host[node.parent] / to_path(node.name);
    const bool fresh = !std::filesystem::exists(host[index], ec);
    if (HasFlags(node.attributes, FileAttributes::kDirectory)) {
      std::filesystem::create_directories(host[index], ec);
      if (ec) {
        return fail(Error::kIO, std::format("cannot create '{}'", path_to_utf8(host[index])));
      }
      if (fresh) {
        written.push_back(host[index]);
      }
      continue;
    }
    std::ofstream stream(host[index], std::ios::binary | std::ios::trunc);
    if (!stream) {
      return fail(Error::kIO, std::format("cannot open '{}'", path_to_utf8(host[index])));
    }
    if (fresh) {
      written.push_back(host[index]);
    }
    chunk.resize(static_cast<size_t>(std::min<u64>(node.size, kExtractChunk)));
    for (u64 offset = 0; offset < node.size;) {
      auto read = Read(node, offset, chunk);
      if (!read || *read == 0) {
        return fail(Error::kCorrupt, std::format("cannot read '{}'", PathOf(node)));
      }
      if (!stream.write(reinterpret_cast<const char*>(chunk.data()),
                        static_cast<std::streamsize>(*read))) {
        return fail(Error::kIO, std::format("cannot write '{}'", path_to_utf8(host[index])));
      }
      offset += *read;
    }
    stream.close();
    if (!stream) {
      return fail(Error::kIO, std::format("cannot flush '{}'", path_to_utf8(host[index])));
    }
  }
  return Ok();
}

}  // namespace rex::filesystem
