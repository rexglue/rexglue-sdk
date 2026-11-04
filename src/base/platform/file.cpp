#include <rex/platform/file.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

#include <rex/string/text.h>

namespace rex::platform {

namespace {

Error IOError(std::string_view what, const std::filesystem::path& path) {
  return Error(ErrorCategory::kIO, std::format("{} '{}'", what, string::PathToUTF8(path)));
}

bool IsDeviceName(std::string_view stem) {
  while (!stem.empty() && stem.back() == ' ') {
    stem.remove_suffix(1);
  }
  for (const std::string_view name : {"CON", "PRN", "AUX", "NUL"}) {
    if (string::utf8_equal_case(stem, name)) {
      return true;
    }
  }
  if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') {
    return string::utf8_equal_case(stem.substr(0, 3), "COM") ||
           string::utf8_equal_case(stem.substr(0, 3), "LPT");
  }
  return false;
}

}  // namespace

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetAppRoot(std::string_view app_name) {
  const std::filesystem::path folder = GetExecutableFolder();
  if (!IsPackagedApp() && IsDirectoryWritable(folder)) {
    return folder;
  }
  return GetUserConfigFolder(app_name);
}

std::string SanitizeFolderName(std::string_view name, std::string_view fallback) {
  std::string out;
  for (const char c : name) {
    if (static_cast<u8>(c) < 0x20 ||
        std::string_view("/\\:<>\"|?*").find(c) != std::string_view::npos) {
      continue;
    }
    out += c;
  }
  const size_t first = out.find_first_not_of(" .");
  if (first == std::string::npos) {
    return std::string(fallback);
  }
  out = out.substr(first, out.find_last_not_of(" .") - first + 1);
  const size_t stem_end = out.find('.');
  if (IsDeviceName(std::string_view(out).substr(0, stem_end))) {
    out.insert(stem_end == std::string::npos ? out.size() : stem_end, 1, '_');
  }
  return out;
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
  std::filesystem::path exact = parent / string::to_path(name);
  if (std::filesystem::exists(std::filesystem::symlink_status(exact, ec))) {
    return exact;
  }
  const std::filesystem::directory_iterator end;
  for (std::filesystem::directory_iterator it(parent, ec); !ec && it != end; it.increment(ec)) {
    if (string::utf8_equal_case(string::PathToUTF8(it->path().filename()), name)) {
      return it->path();
    }
  }
  return exact;
}

bool OpenStorageFile(std::fstream& file, const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    std::ofstream create(path, std::ios::binary);
  }
  file.open(path, std::ios::in | std::ios::out | std::ios::binary);
  return file.is_open();
}

size_t ReadStorageFile(std::fstream& file, void* data, size_t size) {
  file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
  return static_cast<size_t>(file.gcount());
}

void AppendStorageFile(std::fstream& file, const void* data, size_t size) {
  file.seekp(0, std::ios::end);
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void ResetStorageFile(std::fstream& file, const std::filesystem::path& path, u64 length) {
  file.close();
  std::error_code error;
  std::filesystem::resize_file(path, length, error);
  file.open(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(0, std::ios::end);
}

}  // namespace rex::platform
