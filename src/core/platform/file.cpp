#include <rex/platform/file.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

#include <rex/string.h>

namespace rex::platform {

namespace {

Error IOError(std::string_view what, const std::filesystem::path& path) {
  return Error(ErrorCategory::kIO, std::format("{} '{}'", what, PathToUTF8(path)));
}

}  // namespace

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

Result<std::vector<u8>> ReadHostFile(const std::filesystem::path& path, u64 offset, u64 length) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return Err<std::vector<u8>>(IOError("cannot open", path));
  }
  const std::streamoff end = stream.tellg();
  if (end < 0) {
    return Err<std::vector<u8>>(IOError("cannot size", path));
  }
  const u64 size = static_cast<u64>(end);
  if (offset > size) {
    return Err<std::vector<u8>>(IOError("offset is past the end of", path));
  }
  const u64 available = size - offset;
  const u64 count = length == 0 ? available : std::min(length, available);
  std::vector<u8> bytes(static_cast<size_t>(count));
  if (bytes.empty()) {
    return bytes;
  }
  stream.seekg(static_cast<std::streamoff>(offset));
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count))) {
    return Err<std::vector<u8>>(IOError("cannot read", path));
  }
  return bytes;
}

Result<void> WriteHostFile(const std::filesystem::path& path, std::span<const u8> data) {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      return Err(IOError("cannot create the folder of", path));
    }
  }
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Err(IOError("cannot open", temp));
    }
    if (!data.empty() && !stream.write(reinterpret_cast<const char*>(data.data()),
                                       static_cast<std::streamsize>(data.size()))) {
      return Err(IOError("cannot write", temp));
    }
    stream.close();
    if (!stream) {
      return Err(IOError("cannot flush", temp));
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    return Err(IOError("cannot replace", path));
  }
  return Ok();
}

Result<void> WriteHostFile(const std::filesystem::path& path, std::string_view text) {
  return WriteHostFile(path,
                       std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

std::filesystem::path ResolveEntryCase(const std::filesystem::path& parent, std::string_view name) {
  std::error_code ec;
  std::filesystem::path exact = parent / to_path(name);
  if (std::filesystem::exists(std::filesystem::symlink_status(exact, ec))) {
    return exact;
  }
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator it(parent, ec); !ec && it != end; it.increment(ec)) {
    if (utf8_equal_case(PathToUTF8(it->path().filename()), name)) {
      return it->path();
    }
  }
  return exact;
}

}  // namespace rex::platform
