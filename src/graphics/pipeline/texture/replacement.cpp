/**
 * @file        graphics/pipeline/texture/replacement.cpp
 * @brief       Texture replacement packs: the index, the keys, the policy and
 *              the DDS files. See rex/graphics/pipeline/texture/replacement.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>

// Read once, when the GPU starts (the texture cache is created): set them
// before that, for example right after the GPU plugin is loaded.
REXCVAR_DEFINE_STRING(texture_replacement_path, "", "GPU",
                      "Folder of a texture replacement pack (with an index.tsv and DDS files): "
                      "textures whose guest data is in the pack are drawn with the pack's "
                      "images. Empty = off")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(texture_replacement_user_path, "", "GPU",
                      "Folder of a second texture replacement pack (the user's own "
                      "replacements, same layout), read after texture_replacement_path: a "
                      "texture in both comes from this one. Empty = none")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(texture_replacement_log, false, "GPU",
                    "Log the key of every texture that could be replaced, when its host texture "
                    "is created, and whether it was (to check the keys against the pack's)");

// The policy: live, so a game can turn replacement off for a multiplayer
// match (where a replaced texture could be made see-through) and back on.
REXCVAR_DEFINE_BOOL(texture_replacement_enabled, true, "GPU",
                    "Whether textures are replaced from the packs at all. Can change while the "
                    "game runs: textures already replaced go back to the guest's data");

REXCVAR_DEFINE_STRING(texture_replacement_allow_list, "", "GPU",
                      "A file of texture keys (one 16-hex-digit key per line, # comments): while "
                      "set, only those textures are replaced, whatever the packs hold. Can change "
                      "while the game runs. A file that can't be read allows nothing. Empty = "
                      "every texture in the packs");

REXCVAR_DEFINE_INT32(texture_replacement_memory_limit, 4096, "GPU",
                     "Most memory the replacement images alive at once may take, in MB (the "
                     "images are uploaded as they are, so this is about their GPU memory too). A "
                     "texture that would go beyond it is loaded from the guest's data instead. "
                     "0 = no limit");

namespace rex::graphics::texture_replacement {

bool IsFormatSupported(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_8_8_8_8:
      return true;
    default:
      return false;
  }
}

uint32_t GetBlockSize(xenos::TextureFormat format) {
  return format == xenos::TextureFormat::k_8_8_8_8 ? 1 : 4;
}

uint32_t GetBytesPerBlock(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
      return 8;
    case xenos::TextureFormat::k_8_8_8_8:
      return 4;
    default:
      // k_DXT2_3, k_DXT4_5, k_DXN.
      return 16;
  }
}

bool ComputeKey(const GuestBaseLevel& level, std::vector<uint8_t>& scratch, uint64_t& key_out) {
  if (!IsFormatSupported(level.format) || !level.data || !level.width || !level.height) {
    return false;
  }
  const uint32_t block_size = GetBlockSize(level.format);
  const uint32_t bytes_per_block = GetBytesPerBlock(level.format);
  const uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  const uint32_t width_blocks = (level.width + block_size - 1) / block_size;
  const uint32_t height_blocks = (level.height + block_size - 1) / block_size;
  // As the ripper does: a pitch smaller than the width (which the GPU
  // shouldn't see) is taken as the width.
  const uint32_t pitch_blocks = std::max(level.pitch_texels / block_size, width_blocks);
  const uint32_t row_bytes = width_blocks * bytes_per_block;

  scratch.resize(size_t(row_bytes) * height_blocks);
  uint8_t* out = scratch.data();
  for (uint32_t y = 0; y < height_blocks; ++y) {
    // The level's place in its tile: (0, 0), or 16 texels in for a base
    // level packed with its mips.
    const uint32_t tile_y = y + level.y_offset_blocks;
    if (level.tiled) {
      for (uint32_t x = 0; x < width_blocks; ++x) {
        // GetTiledOffset2D aligns the pitch to 32 blocks itself, like the
        // ripper's tiled_offset_2d, and returns a byte offset.
        uint32_t offset = uint32_t(texture_util::GetTiledOffset2D(
            int32_t(x + level.x_offset_blocks), int32_t(tile_y), pitch_blocks,
            bytes_per_block_log2));
        if (offset > level.data_size || level.data_size - offset < bytes_per_block) {
          return false;
        }
        std::memcpy(out, level.data + offset, bytes_per_block);
        out += bytes_per_block;
      }
    } else {
      uint64_t offset =
          (uint64_t(tile_y) * pitch_blocks + level.x_offset_blocks) * bytes_per_block;
      if (offset + row_bytes > level.data_size) {
        return false;
      }
      std::memcpy(out, level.data + offset, row_bytes);
      out += row_bytes;
    }
  }

  // The GPU's byte swap, on the gathered blocks (every block is a multiple of
  // 4 bytes, so swapping the whole buffer is the same as swapping each block).
  uint8_t* bytes = scratch.data();
  const size_t size = scratch.size();
  switch (level.endianness) {
    case xenos::Endian::kNone:
      break;
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(bytes[i], bytes[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(bytes[i], bytes[i + 3]);
        std::swap(bytes[i + 1], bytes[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(bytes[i], bytes[i + 2]);
        std::swap(bytes[i + 1], bytes[i + 3]);
      }
      break;
  }

  key_out = XXH64(scratch.data(), size, 0);
  return true;
}

bool ParseKey(std::string_view text, uint64_t& key_out) {
  if (text.empty() || text.size() > 16) {
    return false;
  }
  uint64_t value = 0;
  for (char c : text) {
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = uint32_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = uint32_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = uint32_t(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  key_out = value;
  return true;
}

bool IsPlainPackFile(std::string_view file) {
  if (file.empty() || file.size() > kMaxPackFileLength || file.front() == '/') {
    return false;
  }
  for (unsigned char c : file) {
    // Backslashes would be separators on Windows (and a trick elsewhere), a
    // colon a drive letter or an alternate data stream.
    if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') {
      return false;
    }
  }
  std::string_view remaining = file;
  while (!remaining.empty()) {
    size_t slash = remaining.find('/');
    std::string_view component = remaining.substr(0, slash);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    remaining = slash == std::string_view::npos ? std::string_view() : remaining.substr(slash + 1);
    if (remaining.empty() && slash != std::string_view::npos) {
      // A trailing slash: a folder, not a file.
      return false;
    }
  }
  return true;
}

bool IsInsideFolder(const std::filesystem::path& folder, const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path folder_real = std::filesystem::canonical(folder, ec);
  if (ec) {
    return false;
  }
  std::filesystem::path path_real = std::filesystem::canonical(path, ec);
  if (ec) {
    return false;
  }
  // Inside: the folder's components are the path's first components.
  auto folder_it = folder_real.begin();
  auto path_it = path_real.begin();
  for (; folder_it != folder_real.end(); ++folder_it, ++path_it) {
    if (path_it == path_real.end() || *path_it != *folder_it) {
      return false;
    }
  }
  return path_it != path_real.end();
}

namespace {

bool ParseUint32(std::string_view text, uint32_t max_value, uint32_t& value_out) {
  if (text.empty() || text.size() > 9) {
    return false;
  }
  uint32_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + uint32_t(c - '0');
  }
  if (value > max_value) {
    return false;
  }
  value_out = value;
  return true;
}

// Calls `visit(line, line_number)` for each line of a text, without the line
// ending (\n or \r\n).
template <typename Visitor>
void ForEachLine(std::string_view text, Visitor visit) {
  size_t line_number = 0;
  while (!text.empty()) {
    size_t line_end = text.find('\n');
    std::string_view line = text.substr(0, line_end);
    text = line_end == std::string_view::npos ? std::string_view() : text.substr(line_end + 1);
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    visit(line, line_number);
  }
}

bool ReadTextFile(const std::filesystem::path& path, uint64_t max_bytes, std::string& contents_out,
                  std::string& error_out) {
  std::error_code ec;
  const uint64_t size = uint64_t(std::filesystem::file_size(path, ec));
  if (ec) {
    error_out = "can't open " + rex::path_to_utf8(path);
    return false;
  }
  if (size > max_bytes) {
    error_out = rex::path_to_utf8(path) + " is " + std::to_string(size >> 20) +
                " MB, larger than such a file may be (" + std::to_string(max_bytes >> 20) +
                " MB)";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error_out = "can't open " + rex::path_to_utf8(path);
    return false;
  }
  contents_out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) {
    error_out = "can't read " + rex::path_to_utf8(path);
    return false;
  }
  return true;
}

}  // namespace

bool Index::Load(const std::filesystem::path& folder, bool strict_alpha, std::string& error_out) {
  std::filesystem::path index_path = folder / "index.tsv";
  std::string contents;
  if (!ReadTextFile(index_path, kMaxIndexFileBytes, contents, error_out)) {
    return false;
  }

  const uint32_t pack = uint32_t(folders_.size());
  size_t usable = 0;
  size_t skipped = 0;
  first_skipped_line_ = 0;
  ForEachLine(contents, [&](std::string_view line, size_t line_number) {
    if (line.empty() || line.front() == '#') {
      return;
    }
    // <key> <width> <height> <xenos_format> <file>, separated by tabs (the
    // file name may contain anything but a tab).
    std::string_view fields[5];
    size_t field_count = 0;
    while (field_count < 5) {
      size_t tab = field_count < 4 ? line.find('\t') : std::string_view::npos;
      fields[field_count++] = line.substr(0, tab);
      if (tab == std::string_view::npos) {
        line = std::string_view();
        break;
      }
      line = line.substr(tab + 1);
    }
    IndexEntry entry;
    uint64_t key;
    uint32_t format_number = 0;
    bool valid = field_count == 5 && fields[0].size() == 16 && ParseKey(fields[0], key) &&
                 ParseUint32(fields[1], xenos::kTexture2DCubeMaxWidthHeight, entry.width) &&
                 ParseUint32(fields[2], xenos::kTexture2DCubeMaxWidthHeight, entry.height) &&
                 entry.width && entry.height && ParseUint32(fields[3], 63, format_number) &&
                 IsFormatSupported(xenos::TextureFormat(format_number)) &&
                 IsPlainPackFile(fields[4]);
    if (!valid) {
      if (!skipped++) {
        first_skipped_line_ = line_number;
      }
      return;
    }
    entry.format = xenos::TextureFormat(format_number);
    entry.file = std::string(fields[4]);
    entry.pack = pack;
    ++usable;
    // Several lines with one key in one pack (the same texture under
    // different names in the game's files) are the same picture: the first
    // one is used. A key in a pack read earlier is overridden.
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      entries_.emplace(key, std::move(entry));
    } else if (it->second.pack != pack) {
      it->second = std::move(entry);
    }
  });
  skipped_lines_ += skipped;

  if (!usable) {
    error_out = rex::path_to_utf8(index_path) + " has no usable line";
    return false;
  }
  folders_.push_back(folder);
  strict_alpha_.push_back(strict_alpha);
  return true;
}

const IndexEntry* Index::Find(uint64_t key) const {
  auto it = entries_.find(key);
  return it != entries_.end() ? &it->second : nullptr;
}

std::filesystem::path Index::FilePath(const IndexEntry& entry) const {
  return folders_[entry.pack] / rex::to_path(entry.file);
}

namespace {

// The policy, read by the texture cache (GPU thread) and written by the
// setting callbacks (any thread).
std::mutex policy_mutex;
Policy policy_current;
std::atomic<uint32_t> policy_generation{0};

// Rebuilds the policy from the settings (under policy_mutex) and bumps the
// generation. The allow list file is read here, once per change.
void UpdatePolicy() {
  Policy policy;
  policy.enabled = REXCVAR_GET(texture_replacement_enabled);
  const std::string allow_path = REXCVAR_GET(texture_replacement_allow_list);
  if (!allow_path.empty()) {
    auto keys = std::make_shared<std::unordered_set<uint64_t>>();
    size_t malformed = 0;
    std::string error;
    if (LoadAllowList(rex::to_path(allow_path), *keys, malformed, error)) {
      REXGPU_INFO("Texture replacement: only the {} texture(s) listed in {} are replaced{}",
                  keys->size(), allow_path,
                  malformed ? fmt::format(" ({} malformed line(s) skipped)", malformed) : "");
    } else {
      // Fails closed: a list that can't be read allows nothing.
      REXGPU_WARN("Texture replacement: {}; no texture is replaced while the allow list is set",
                  error);
    }
    policy.allow_list = std::move(keys);
  }
  if (!policy.enabled) {
    REXGPU_INFO("Texture replacement: off (texture_replacement_enabled)");
  } else if (!policy.allow_list) {
    REXGPU_INFO("Texture replacement: on for every texture in the packs");
  }
  std::lock_guard<std::mutex> lock(policy_mutex);
  policy.generation = policy_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  policy_current = std::move(policy);
}

}  // namespace

const Index* GetIndex() {
  static std::once_flag once;
  static std::unique_ptr<Index> index;
  std::call_once(once, [] {
    // The main pack, then the user's own, which overrides it and whose
    // images are checked against the guest's alpha.
    const std::string paths[] = {REXCVAR_GET(texture_replacement_path),
                                 REXCVAR_GET(texture_replacement_user_path)};
    const bool strict_alpha[] = {false, true};
    auto loaded = std::make_unique<Index>();
    for (size_t i = 0; i < 2; ++i) {
      const std::string& path = paths[i];
      if (path.empty()) {
        continue;
      }
      std::string error;
      size_t before = loaded->size();
      if (!loaded->Load(rex::to_path(path), strict_alpha[i], error)) {
        REXGPU_WARN("Texture replacement: {}; the pack at {} isn't used", error, path);
        continue;
      }
      REXGPU_INFO("Texture replacement: {} textures in the pack at {} ({} new)", loaded->size(),
                  path, loaded->size() - before);
      if (loaded->first_skipped_line()) {
        REXGPU_WARN("Texture replacement: skipped malformed line(s) of {}/index.tsv (first: line {})",
                    path, loaded->first_skipped_line());
      }
    }
    if (!loaded->pack_count()) {
      if (!paths[0].empty() || !paths[1].empty()) {
        REXGPU_WARN("Texture replacement: no usable pack; texture replacement is off");
      }
      return;
    }
    index = std::move(loaded);
    // The policy follows its settings from now on.
    UpdatePolicy();
    for (const char* name : {"texture_replacement_enabled", "texture_replacement_allow_list"}) {
      rex::cvar::RegisterChangeCallback(name,
                                        [](std::string_view, std::string_view) { UpdatePolicy(); });
    }
  });
  return index.get();
}

Policy GetPolicy() {
  std::lock_guard<std::mutex> lock(policy_mutex);
  return policy_current;
}

uint32_t GetPolicyGeneration() { return policy_generation.load(std::memory_order_acquire); }

bool LoadAllowList(const std::filesystem::path& path, std::unordered_set<uint64_t>& keys_out,
                   size_t& malformed_lines_out, std::string& error_out) {
  std::string contents;
  if (!ReadTextFile(path, kMaxAllowListFileBytes, contents, error_out)) {
    return false;
  }
  malformed_lines_out = 0;
  ForEachLine(contents, [&](std::string_view line, size_t) {
    // The key is the first word; anything after whitespace is a comment (the
    // texture's name, say).
    size_t end = line.find_first_of(" \t");
    std::string_view word = line.substr(0, end);
    if (word.empty() || word.front() == '#') {
      return;
    }
    uint64_t key;
    if (ParseKey(word, key)) {
      keys_out.insert(key);
    } else {
      ++malformed_lines_out;
    }
  });
  return true;
}

uint64_t GetMemoryLimitBytes() {
  int32_t mb = REXCVAR_GET(texture_replacement_memory_limit);
  return mb > 0 ? uint64_t(mb) << 20 : 0;
}

namespace {

// The legacy DDS header (after the "DDS " magic), as in the DirectX docs.
constexpr uint32_t kDdsMagic = 0x20534444;  // "DDS "
constexpr uint32_t kDdsHeaderSize = 124;
constexpr size_t kDdsFileHeaderSize = 4 + kDdsHeaderSize;
constexpr uint32_t kDdsPixelFormatSize = 32;
constexpr uint32_t kDdsdMipMapCount = 0x20000;
constexpr uint32_t kDdpfAlphaPixels = 0x1;
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdscaps2Cubemap = 0x200;
constexpr uint32_t kDdscaps2Volume = 0x200000;

constexpr uint32_t MakeFourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}

uint32_t ReadLE32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}

// The byte (0 to 3) of a 32-bit little-endian texel that a channel mask
// selects, or -1 if the mask isn't exactly one whole byte.
int MaskByte(uint32_t mask) {
  for (int i = 0; i < 4; ++i) {
    if (mask == (UINT32_C(0xFF) << (8 * i))) {
      return i;
    }
  }
  return -1;
}

const char* FormatName(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
      return "DXT1";
    case xenos::TextureFormat::k_DXT2_3:
      return "DXT3";
    case xenos::TextureFormat::k_DXT4_5:
      return "DXT5";
    case xenos::TextureFormat::k_DXN:
      return "ATI2";
    case xenos::TextureFormat::k_8_8_8_8:
      return "32-bit RGBA";
    default:
      return "?";
  }
}

// The bytes of level `level` of a width x height image in the format: rows of
// whole blocks, no padding.
uint64_t LevelBytes(xenos::TextureFormat format, uint32_t width, uint32_t height,
                    uint32_t level) {
  const uint32_t block_size = GetBlockSize(format);
  const uint32_t level_width = std::max(width >> level, UINT32_C(1));
  const uint32_t level_height = std::max(height >> level, UINT32_C(1));
  const uint64_t row_blocks = (level_width + block_size - 1) / block_size;
  const uint64_t rows = (level_height + block_size - 1) / block_size;
  return row_blocks * rows * GetBytesPerBlock(format);
}

}  // namespace

bool IsReplacementSize(uint32_t guest_width, uint32_t guest_height, uint32_t width,
                       uint32_t height) {
  if (!guest_width || !guest_height || !width || !height) {
    return false;
  }
  // With width = s * guest_width + e1 and height = s * guest_height + e2,
  // |e1|, |e2| <= 4: |width * guest_height - height * guest_width| =
  // |e1 * guest_height - e2 * guest_width| <= 4 * (guest_width + guest_height).
  uint64_t a = uint64_t(width) * guest_height;
  uint64_t b = uint64_t(height) * guest_width;
  return (a > b ? a - b : b - a) <= 4 * (uint64_t(guest_width) + guest_height);
}

uint32_t GetReplacementLevelCount(uint32_t guest_width, uint32_t guest_height,
                                  uint32_t guest_level_count, uint32_t width, uint32_t height) {
  uint32_t longest = std::max(width, height);
  uint32_t guest_longest = std::max(std::max(guest_width, guest_height), UINT32_C(1));
  // Not a whole number in general (3x, 2.5x ...): the extra levels are the
  // nearest whole number of halvings that brings the image to the guest's
  // size, so the smallest level comes as close as it can to the guest's.
  double scale = double(longest) / double(guest_longest);
  long extra = scale > 1.0 ? std::lround(std::log2(scale)) : 0;
  uint32_t full_chain = rex::log2_floor(std::max(longest, UINT32_C(1))) + 1;
  return std::min(full_chain,
                  std::max(guest_level_count, UINT32_C(1)) + uint32_t(std::max(extra, 0L)));
}

bool ParseDdsHeader(const uint8_t* header, size_t header_size, DdsInfo& info_out,
                    std::string& error_out) {
  if (!header || header_size < kDdsFileHeaderSize) {
    error_out = "the file is too short for a DDS header";
    return false;
  }
  const uint8_t* dds = header + 4;
  const uint8_t* pixel_format = dds + 72;
  if (ReadLE32(header) != kDdsMagic || ReadLE32(dds) != kDdsHeaderSize ||
      ReadLE32(pixel_format) != kDdsPixelFormatSize) {
    error_out = "not a DDS file";
    return false;
  }
  const uint32_t flags = ReadLE32(dds + 4);
  const uint32_t height = ReadLE32(dds + 8);
  const uint32_t width = ReadLE32(dds + 12);
  const uint32_t mip_map_count = ReadLE32(dds + 24);
  const uint32_t pixel_format_flags = ReadLE32(pixel_format + 4);
  const uint32_t four_cc = ReadLE32(pixel_format + 8);
  const uint32_t caps2 = ReadLE32(dds + 108);

  DdsInfo info;
  if (pixel_format_flags & kDdpfFourCC) {
    switch (four_cc) {
      case MakeFourCC('D', 'X', 'T', '1'):
        info.format = xenos::TextureFormat::k_DXT1;
        break;
      // DXT2 and DXT4 are the premultiplied-alpha names of the same blocks.
      case MakeFourCC('D', 'X', 'T', '2'):
      case MakeFourCC('D', 'X', 'T', '3'):
        info.format = xenos::TextureFormat::k_DXT2_3;
        break;
      case MakeFourCC('D', 'X', 'T', '4'):
      case MakeFourCC('D', 'X', 'T', '5'):
        info.format = xenos::TextureFormat::k_DXT4_5;
        break;
      case MakeFourCC('A', 'T', 'I', '2'):
      case MakeFourCC('B', 'C', '5', 'U'):
        info.format = xenos::TextureFormat::k_DXN;
        break;
      case MakeFourCC('D', 'X', '1', '0'):
        error_out = "DDS files with a DX10 header aren't supported";
        return false;
      default:
        error_out = "unsupported DDS format (FourCC)";
        return false;
    }
  } else if ((pixel_format_flags & kDdpfRgb) && ReadLE32(pixel_format + 12) == 32) {
    info.format = xenos::TextureFormat::k_8_8_8_8;
    // Any order of whole-byte channels is accepted and rearranged into B, G,
    // R, A, the memory order of the guest's A8R8G8B8 textures after the
    // byte swap; the pack's ripper writes that order already (the usual
    // masks 0xFF0000, 0xFF00, 0xFF, 0xFF000000).
    info.has_alpha = (pixel_format_flags & kDdpfAlphaPixels) != 0;
    int r = MaskByte(ReadLE32(pixel_format + 16));
    int g = MaskByte(ReadLE32(pixel_format + 20));
    int b = MaskByte(ReadLE32(pixel_format + 24));
    int a = info.has_alpha ? MaskByte(ReadLE32(pixel_format + 28)) : -1;
    if (r < 0 || g < 0 || b < 0 || (info.has_alpha && a < 0) || r == g || r == b || g == b ||
        (info.has_alpha && (a == r || a == g || a == b))) {
      error_out = "unsupported 32-bit DDS channel masks";
      return false;
    }
    if (!info.has_alpha) {
      // The byte left over (filled with opaque alpha by the loader).
      a = 0 + 1 + 2 + 3 - r - g - b;
    }
    info.bgra_source_bytes[0] = b;
    info.bgra_source_bytes[1] = g;
    info.bgra_source_bytes[2] = r;
    info.bgra_source_bytes[3] = a;
  } else {
    error_out = "unsupported DDS pixel format";
    return false;
  }
  if (caps2 & (kDdscaps2Cubemap | kDdscaps2Volume)) {
    error_out = "cube maps and volume textures aren't supported";
    return false;
  }
  if (!width || !height || width > kMaxImageWidthHeight || height > kMaxImageWidthHeight) {
    error_out = "the image is " + std::to_string(width) + "x" + std::to_string(height) +
                ", not between 1x1 and " + std::to_string(kMaxImageWidthHeight) + "x" +
                std::to_string(kMaxImageWidthHeight);
    return false;
  }
  info.width = width;
  info.height = height;
  // The levels: the header's count (1 if it has none), which can't be more
  // than a full chain of the image.
  const uint32_t full_chain = rex::log2_floor(std::max(width, height)) + 1;
  info.level_count = (flags & kDdsdMipMapCount) && mip_map_count ? mip_map_count : 1;
  if (info.level_count > kMaxImageLevels || info.level_count > full_chain) {
    error_out = "the header declares " + std::to_string(info.level_count) +
                " mip levels, more than a " + std::to_string(width) + "x" +
                std::to_string(height) + " image can have";
    return false;
  }
  info.data_size = 0;
  for (uint32_t i = 0; i < info.level_count; ++i) {
    info.data_size += LevelBytes(info.format, width, height, i);
  }
  info_out = info;
  return true;
}

bool LoadDds(const std::filesystem::path& path, xenos::TextureFormat guest_format,
             uint32_t guest_width, uint32_t guest_height, uint32_t guest_level_count,
             uint32_t max_width_height, uint64_t max_bytes, Image& image_out,
             std::string& error_out, bool* too_large_out) {
  if (too_large_out) {
    *too_large_out = false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error_out = "can't open the file";
    return false;
  }
  uint8_t header[kDdsFileHeaderSize];
  if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) {
    error_out = "the file is too short for a DDS header";
    return false;
  }
  DdsInfo info;
  if (!ParseDdsHeader(header, sizeof(header), info, error_out)) {
    return false;
  }

  // The file's format: it must be the guest texture's.
  if (info.format != guest_format) {
    error_out = std::string("the file is ") + FormatName(info.format) + ", the texture " +
                FormatName(guest_format);
    return false;
  }

  // The size: any, as long as it's the texture's scaled (the scale isn't
  // fixed, and needn't be a whole number) and the host can create it.
  const uint32_t width = info.width;
  const uint32_t height = info.height;
  const std::string size_text = std::to_string(width) + "x" + std::to_string(height);
  if (!IsReplacementSize(guest_width, guest_height, width, height)) {
    error_out = "the image is " + size_text + ", which isn't the texture's " +
                std::to_string(guest_width) + "x" + std::to_string(guest_height) +
                " scaled (another aspect ratio)";
    return false;
  }
  if (width > max_width_height || height > max_width_height) {
    error_out = "the image is " + size_text + ", larger than the GPU's limit of " +
                std::to_string(max_width_height);
    return false;
  }

  // The file must hold exactly the levels its header declares: a short file
  // is broken, a long one isn't what the pack's ripper writes.
  std::error_code ec;
  const uint64_t file_size = uint64_t(std::filesystem::file_size(path, ec));
  if (ec) {
    error_out = "can't get the file's size";
    return false;
  }
  if (file_size != kDdsFileHeaderSize + info.data_size) {
    error_out = "the file is " + std::to_string(file_size) + " bytes, but its header (" +
                size_text + ", " + std::to_string(info.level_count) + " level(s)) means " +
                std::to_string(kDdsFileHeaderSize + info.data_size);
    return false;
  }

  // The levels: the file's, but not past the guest's smallest level.
  uint32_t level_count = std::min(
      info.level_count,
      GetReplacementLevelCount(guest_width, guest_height, guest_level_count, width, height));

  Image& image = image_out;
  image.format = info.format;
  image.block_size = GetBlockSize(info.format);
  image.width = width;
  image.height = height;
  image.levels.clear();
  uint64_t total_size = 0;
  const uint32_t bytes_per_block = GetBytesPerBlock(info.format);
  for (uint32_t i = 0; i < level_count; ++i) {
    Image::Level level;
    level.width = std::max(width >> i, UINT32_C(1));
    level.height = std::max(height >> i, UINT32_C(1));
    level.row_bytes = (level.width + image.block_size - 1) / image.block_size * bytes_per_block;
    level.row_count = (level.height + image.block_size - 1) / image.block_size;
    level.offset = size_t(total_size);
    total_size += uint64_t(level.row_bytes) * level.row_count;
    image.levels.push_back(level);
  }
  if (max_bytes && total_size > max_bytes) {
    if (too_large_out) {
      *too_large_out = true;
    }
    error_out = "its " + std::to_string(total_size) +
                " bytes would go beyond the replacement memory limit";
    return false;
  }
  image.data.resize(size_t(total_size));
  // The levels follow the header, largest first, without padding.
  if (!file.read(reinterpret_cast<char*>(image.data.data()), std::streamsize(total_size))) {
    error_out = "the file is shorter than its " + std::to_string(level_count) + " level(s)";
    return false;
  }

  if (info.format == xenos::TextureFormat::k_8_8_8_8 &&
      (info.bgra_source_bytes[0] != 0 || info.bgra_source_bytes[1] != 1 ||
       info.bgra_source_bytes[2] != 2 || info.bgra_source_bytes[3] != 3 || !info.has_alpha)) {
    for (size_t i = 0; i + 3 < image.data.size(); i += 4) {
      uint8_t* texel = image.data.data() + i;
      uint8_t source[4] = {texel[0], texel[1], texel[2], texel[3]};
      for (int j = 0; j < 4; ++j) {
        texel[j] = source[info.bgra_source_bytes[j]];
      }
      if (!info.has_alpha) {
        texel[3] = 0xFF;
      }
    }
  }
  return true;
}

namespace {

uint16_t ReadLE16(const uint8_t* bytes) { return uint16_t(bytes[0] | (bytes[1] << 8)); }

uint64_t ReadLE48(const uint8_t* bytes) {
  uint64_t value = 0;
  for (int i = 5; i >= 0; --i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

// The 16 alphas of a block (texels row by row), for the BC formats with
// alpha.
void BlockAlphas(xenos::TextureFormat format, const uint8_t* block, uint8_t alphas_out[16]) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1: {
      // Three-color mode (color0 <= color1): index 3 is transparent black.
      const bool three_color = ReadLE16(block) <= ReadLE16(block + 2);
      const uint32_t indices = ReadLE32(block + 4);
      for (uint32_t i = 0; i < 16; ++i) {
        alphas_out[i] = three_color && ((indices >> (2 * i)) & 3) == 3 ? 0 : 255;
      }
      break;
    }
    case xenos::TextureFormat::k_DXT2_3:
      // Explicit 4-bit alphas, the low nibble first.
      for (uint32_t i = 0; i < 16; ++i) {
        uint32_t nibble = (block[i >> 1] >> ((i & 1) * 4)) & 0xF;
        alphas_out[i] = uint8_t(nibble * 17);
      }
      break;
    case xenos::TextureFormat::k_DXT4_5: {
      // Two end points and 3-bit indices into 8 values between them.
      const uint32_t a0 = block[0];
      const uint32_t a1 = block[1];
      uint8_t values[8] = {uint8_t(a0), uint8_t(a1)};
      if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i) {
          values[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
        }
      } else {
        for (uint32_t i = 1; i < 5; ++i) {
          values[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
        }
        values[6] = 0;
        values[7] = 255;
      }
      const uint64_t indices = ReadLE48(block + 2);
      for (uint32_t i = 0; i < 16; ++i) {
        alphas_out[i] = values[(indices >> (3 * i)) & 7];
      }
      break;
    }
    default:
      std::memset(alphas_out, 255, 16);
      break;
  }
}

}  // namespace

bool GetAlphaStats(xenos::TextureFormat format, uint32_t width, uint32_t height,
                   const uint8_t* data, size_t data_size, AlphaStats& stats_out) {
  AlphaStats stats;
  if (!IsFormatSupported(format) || !data || !width || !height ||
      data_size < LevelBytes(format, width, height, 0)) {
    return false;
  }
  if (format == xenos::TextureFormat::k_DXN) {
    // Red and green only: no alpha.
    stats.texels = stats.visible = uint64_t(width) * height;
    stats.alpha_sum = stats.texels * 255;
    stats_out = stats;
    return true;
  }
  auto add = [&stats](uint8_t alpha) {
    ++stats.texels;
    stats.visible += alpha >= 128;
    stats.alpha_sum += alpha;
    stats.opaque = stats.opaque && alpha == 255;
  };
  if (format == xenos::TextureFormat::k_8_8_8_8) {
    // Texels B, G, R, A (the byte order after the GPU's swap).
    for (uint64_t i = 0; i < uint64_t(width) * height; ++i) {
      add(data[i * 4 + 3]);
    }
    stats_out = stats;
    return true;
  }
  const uint32_t width_blocks = (width + 3) / 4;
  const uint32_t height_blocks = (height + 3) / 4;
  const uint32_t block_bytes = GetBytesPerBlock(format);
  uint8_t alphas[16];
  for (uint32_t by = 0; by < height_blocks; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      BlockAlphas(format, data + (size_t(by) * width_blocks + bx) * block_bytes, alphas);
      // Only the texels inside the image count (an edge block's others are
      // padding).
      for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
        for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
          add(alphas[y * 4 + x]);
        }
      }
    }
  }
  stats_out = stats;
  return true;
}

uint64_t ForceOpaque(Image& image) {
  uint64_t changed = 0;
  for (size_t level_index = 0; level_index < image.levels.size(); ++level_index) {
    const Image::Level& level = image.levels[level_index];
    uint8_t* data = image.data.data() + level.offset;
    const size_t size = size_t(level.row_bytes) * level.row_count;
    switch (image.format) {
      case xenos::TextureFormat::k_DXT1:
        for (size_t at = 0; at + 8 <= size; at += 8) {
          uint8_t* block = data + at;
          if (ReadLE16(block) > ReadLE16(block + 2)) {
            continue;  // Four-color mode: opaque.
          }
          // In the three-color mode, index 2 is the middle color (opaque) and
          // index 3 transparent black: draw the transparent texels with the
          // middle color.
          uint32_t indices = ReadLE32(block + 4);
          uint32_t new_indices = indices;
          for (uint32_t i = 0; i < 16; ++i) {
            if (((indices >> (2 * i)) & 3) == 3) {
              new_indices &= ~(UINT32_C(1) << (2 * i));  // 3 -> 2
            }
          }
          if (new_indices != indices) {
            for (int b = 0; b < 4; ++b) {
              block[4 + b] = uint8_t(new_indices >> (8 * b));
            }
            ++changed;
          }
        }
        break;
      case xenos::TextureFormat::k_DXT2_3:
        for (size_t at = 0; at + 16 <= size; at += 16) {
          uint8_t* block = data + at;
          bool opaque = true;
          for (int b = 0; b < 8; ++b) {
            opaque = opaque && block[b] == 0xFF;
          }
          if (!opaque) {
            std::memset(block, 0xFF, 8);
            ++changed;
          }
        }
        break;
      case xenos::TextureFormat::k_DXT4_5:
        for (size_t at = 0; at + 16 <= size; at += 16) {
          uint8_t* block = data + at;
          uint8_t alphas[16];
          BlockAlphas(image.format, block, alphas);
          bool opaque = true;
          for (uint8_t alpha : alphas) {
            opaque = opaque && alpha == 255;
          }
          if (!opaque) {
            // Both end points 255 and every index 0: alpha 255.
            block[0] = 0xFF;
            block[1] = 0xFF;
            std::memset(block + 2, 0, 6);
            ++changed;
          }
        }
        break;
      case xenos::TextureFormat::k_8_8_8_8:
        // The alpha byte is the texel's last (B, G, R, A).
        for (size_t at = 0; at + 4 <= size; at += 4) {
          if (data[at + 3] != 0xFF) {
            data[at + 3] = 0xFF;
            ++changed;
          }
        }
        break;
      default:
        // k_DXN: no alpha.
        break;
    }
  }
  return changed;
}

bool EnforceAlpha(uint32_t guest_width, uint32_t guest_height, const uint8_t* guest,
                  size_t guest_size, Image& image, uint64_t& forced_out, std::string& error_out) {
  forced_out = 0;
  if (image.format == xenos::TextureFormat::k_DXN) {
    return true;
  }
  AlphaStats guest_stats;
  if (!GetAlphaStats(image.format, guest_width, guest_height, guest, guest_size, guest_stats)) {
    error_out = "the original texture's alpha couldn't be read";
    return false;
  }
  if (guest_stats.opaque) {
    forced_out = ForceOpaque(image);
    return true;
  }
  if (image.levels.empty()) {
    error_out = "the image has no levels";
    return false;
  }
  AlphaStats image_stats;
  const Image::Level& top = image.levels[0];
  if (!GetAlphaStats(image.format, top.width, top.height, image.level_data(0),
                     size_t(top.row_bytes) * top.row_count, image_stats)) {
    error_out = "the image's alpha couldn't be read";
    return false;
  }
  // Compared as shares of the texels (the sizes differ), with 2% for the
  // resampling and compression of a faithful copy.
  const double guest_visible = double(guest_stats.visible) / double(guest_stats.texels);
  const double image_visible = double(image_stats.visible) / double(image_stats.texels);
  const double guest_mean = double(guest_stats.alpha_sum) / double(guest_stats.texels);
  const double image_mean = double(image_stats.alpha_sum) / double(image_stats.texels);
  if (image_visible < guest_visible - 0.02 || image_mean < guest_mean - 0.02 * 255.0) {
    error_out = fmt::format(
        "the image is more see-through than the original ({:.1f}% of its texels are visible, "
        "mean alpha {:.0f}; the original: {:.1f}%, {:.0f})",
        image_visible * 100.0, image_mean, guest_visible * 100.0, guest_mean);
    return false;
  }
  return true;
}

}  // namespace rex::graphics::texture_replacement
