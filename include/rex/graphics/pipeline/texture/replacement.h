/**
 * @file        rex/graphics/pipeline/texture/replacement.h
 *
 * @brief       Texture replacement: host textures made from image files (a
 *              "pack", for example upscaled textures) instead of from the
 *              guest's own texture data.
 *
 * A pack is a folder with an `index.tsv` and DDS files. Each index line is
 *
 *     <key>\t<width>\t<height>\t<xenos_format>\t<file>
 *
 * where `key` is 16 hex digits: XXH64 (seed 0) of the guest texture's base
 * level, untiled into tightly packed rows of blocks and byte-swapped the way
 * the GPU reads it (see ComputeKey); `width`, `height` and `xenos_format` are
 * the guest texture's; `file` is a DDS file relative to the folder: a plain
 * relative path (IsPlainPackFile), never absolute, with no `..`. Lines
 * starting with `#` are comments.
 *
 * Two packs may be set: `texture_replacement_path` (the main pack, for example
 * the upscaled textures) and `texture_replacement_user_path` (the user's own
 * replacements). Both are read into one index; a key in both comes from the
 * user pack.
 *
 * The DDS holds the same picture, usually larger, at any size: no scale is
 * assumed (an upscaler or its model may pick any, and it needn't be a whole
 * number), only the guest's aspect ratio to within a few texels of rounding
 * (IsReplacementSize). It's in the guest format's native host layout:
 * BC1/BC2/BC3/BC5 blocks for k_DXT1/k_DXT2_3/k_DXT4_5/k_DXN, and for
 * k_8_8_8_8 32-bit texels in the byte order the guest data has after the
 * GPU's byte swap (that order goes into the host's R8G8B8A8 texture as it
 * is, like the guest data does; the fetch constant's swizzle then sorts the
 * channels out, as for the guest's own data): for Direct3D's A8R8G8B8, the
 * common case and the one the packs are made for, that is B, G, R, A in
 * memory, which is what a 32-bit DDS with the usual masks holds. Files
 * with other whole-byte masks are rearranged into that order. With its own
 * mip chain. Only legacy DDS
 * headers are read (no DX10 header), and only files whose length is exactly
 * the header plus the levels the header declares (LoadDds checks every
 * value before anything is allocated: a broken or hostile file is skipped
 * with one log line, never trusted).
 *
 * A larger host texture samples like the guest's: shaders get the guest's
 * size from the fetch constant (unnormalized coordinates and texel offsets
 * stay in guest texels), and the GPU picks the level from the host size, so
 * at any scale it uses the level with the guest's texel density.
 *
 * The texture cache (TextureCache::FindOrCreateTexture) computes the key of
 * eligible textures from the guest memory when it's about to create a host
 * texture for them, and creates the texture from the pack's image when the
 * index has the key and the policy allows it.
 *
 * Images from the user pack, and every image while an allow list is in force,
 * are also checked against the guest texture's alpha (EnforceAlpha): a
 * replacement for an opaque texture is made opaque, and one for a texture
 * with transparent parts may not be noticeably more see-through.
 *
 * The policy is live (GetPolicy): `texture_replacement_enabled` turns all
 * replacement off and on, and `texture_replacement_allow_list` names a file
 * of keys (one 16-hex-digit key per line, `#` comments) outside of which no
 * texture is replaced, for example in a multiplayer match, where only a
 * short list of textures (the insignia the player may replace) is allowed
 * because any other replacement could be made transparent or bright to see
 * players through walls. A change bumps the policy generation; the texture
 * cache retires textures whose replacement decision no longer holds the next
 * time they're looked up, so the switch takes effect without a restart.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_replacement {

// The guest formats a pack can replace (the formats CoD-era Xbox 360 games
// keep most of their textures in): k_DXT1, k_DXT2_3, k_DXT4_5, k_DXN and
// k_8_8_8_8.
bool IsFormatSupported(xenos::TextureFormat format);

// Texels per block along each axis (4 for the BC formats, 1 for k_8_8_8_8)
// and bytes per block, for a supported format.
uint32_t GetBlockSize(xenos::TextureFormat format);
uint32_t GetBytesPerBlock(xenos::TextureFormat format);

// The guest base level a key is computed from.
struct GuestBaseLevel {
  // The guest memory at the base address, as the guest wrote it (not
  // byte-swapped), and how many bytes may be read from there.
  const uint8_t* data = nullptr;
  uint32_t data_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // The fetch constant's pitch, in texels (a multiple of 32).
  uint32_t pitch_texels = 0;
  // Where the level's first block is, in blocks: (0, 0) unless the base level
  // lies in the packed mip tile (packed mips and a side of 16 texels or
  // less), where it is 16 texels in (texture_util::GetPackedMipOffset for
  // level 0; the first rows of the tile hold the smaller levels).
  uint32_t x_offset_blocks = 0;
  uint32_t y_offset_blocks = 0;
  bool tiled = false;
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  xenos::Endian endianness = xenos::Endian::kNone;
};

// Computes the key of a base level: untiles it (XGAddress2DTiledOffset, the
// same as texture_util::GetTiledOffset2D, with a pitch of max(pitch, width) in
// blocks, from its offset in the tile) or reads it row by row if it's linear,
// into rows of blocks without padding, byte-swaps that per the endianness, and
// hashes it (XXH64, seed 0). This is exactly what the pack's ripper does with
// the texture data it finds in the game's files, so the keys match. `scratch`
// is reused between calls. Returns false if a block would be read from beyond
// data_size.
bool ComputeKey(const GuestBaseLevel& level, std::vector<uint8_t>& scratch, uint64_t& key_out);

// Parses 1 to 16 hex digits (either case) into a key.
bool ParseKey(std::string_view text, uint64_t& key_out);

// Whether an index line's `file` may be used: a relative path inside the pack
// folder, with `/` between components (no backslashes), no empty, `.` or `..`
// component, no drive letter or other `:`, no control characters, and at most
// kMaxPackFileLength bytes. Symbolic links that lead out of the folder are
// caught later, when the file is opened (IsInsideFolder).
constexpr size_t kMaxPackFileLength = 255;
bool IsPlainPackFile(std::string_view file);

// Whether `path` (an existing file) resolves, symbolic links followed, to a
// place inside `folder`. False if either can't be resolved.
bool IsInsideFolder(const std::filesystem::path& folder, const std::filesystem::path& path);

// One line of index.tsv.
struct IndexEntry {
  // The guest texture's size and format.
  uint32_t width = 0;
  uint32_t height = 0;
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  // The DDS file, relative to the pack folder (UTF-8, IsPlainPackFile).
  std::string file;
  // Which pack it's in (Index::folder).
  uint32_t pack = 0;
};

// The largest index.tsv or allow list read (a pack of every texture of a
// game is a few MB); a larger file is refused rather than read into memory.
constexpr uint64_t kMaxIndexFileBytes = UINT64_C(64) << 20;
constexpr uint64_t kMaxAllowListFileBytes = UINT64_C(1) << 20;

class Index {
 public:
  // Reads <folder>/index.tsv into the index (pack number = the number of
  // packs read before). Returns false, with the reason in error_out, if it
  // can't be read, is larger than kMaxIndexFileBytes or has no usable line.
  // Malformed lines are skipped and counted (skipped_lines). A key the index
  // already has is replaced, so a pack read later overrides one read
  // earlier. `strict_alpha`: the pack's images are checked against the
  // guest's alpha (EnforceAlpha) - for packs made from the user's own
  // images.
  bool Load(const std::filesystem::path& folder, bool strict_alpha, std::string& error_out);

  const IndexEntry* Find(uint64_t key) const;

  // The full path of an entry's DDS file.
  std::filesystem::path FilePath(const IndexEntry& entry) const;
  const std::filesystem::path& folder(uint32_t pack) const { return folders_[pack]; }
  bool strict_alpha(uint32_t pack) const { return strict_alpha_[pack]; }
  size_t pack_count() const { return folders_.size(); }
  size_t size() const { return entries_.size(); }
  size_t skipped_lines() const { return skipped_lines_; }
  // The first skipped line (1-based) of the last pack read, 0 if none.
  size_t first_skipped_line() const { return first_skipped_line_; }

 private:
  std::vector<std::filesystem::path> folders_;
  std::vector<bool> strict_alpha_;
  std::unordered_map<uint64_t, IndexEntry> entries_;
  size_t skipped_lines_ = 0;
  size_t first_skipped_line_ = 0;
};

// The packs in the texture_replacement_path and texture_replacement_user_path
// settings, read the first time this is called (the texture cache calls it
// when it's created, so the settings must be set before the GPU starts). The
// index is read once per run: it's shared by texture caches created later
// (for example after a draw resolution scale change). nullptr if both
// settings are empty or no index is usable (logged once).
const Index* GetIndex();

// The live policy: whether replacement is on at all, and which keys may be
// replaced (every key the index has, or only those in the allow list file).
// `generation` changes whenever the policy does.
struct Policy {
  bool enabled = true;
  // nullptr: every key is allowed. Otherwise only the keys in the set (which
  // may be empty: an allow list file that can't be read allows nothing, so a
  // missing file fails closed).
  std::shared_ptr<const std::unordered_set<uint64_t>> allow_list;
  uint32_t generation = 0;

  bool Allows(uint64_t key) const {
    return enabled && (!allow_list || allow_list->count(key) != 0);
  }
};

// The current policy (a snapshot: a change after this call isn't seen in it).
Policy GetPolicy();
// The current policy generation: cheap (one atomic load), for checking
// whether a texture's decision was made under the current policy.
uint32_t GetPolicyGeneration();

// Reads an allow list file: one key per line (ParseKey), `#` comments and
// blank lines ignored; anything else is a malformed line (counted, skipped).
// Returns false with the reason in error_out if the file can't be read.
bool LoadAllowList(const std::filesystem::path& path, std::unordered_set<uint64_t>& keys_out,
                   size_t& malformed_lines_out, std::string& error_out);

// A replacement image read from a DDS file: levels largest first, each as
// rows of blocks (BC formats: rows of 4x4 blocks; k_8_8_8_8: rows of texels)
// without padding, ready to be copied to a host texture.
struct Image {
  struct Level {
    // In texels.
    uint32_t width = 0;
    uint32_t height = 0;
    // Bytes in one row of blocks, and the number of rows of blocks.
    uint32_t row_bytes = 0;
    uint32_t row_count = 0;
    // Where the level is in `data`, row_bytes * row_count bytes.
    size_t offset = 0;
  };
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  // Texels per block along each axis (4 or 1).
  uint32_t block_size = 1;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<Level> levels;
  std::vector<uint8_t> data;

  const uint8_t* level_data(size_t level) const { return data.data() + levels[level].offset; }
};

// Whether a width x height image may stand for a guest texture of
// guest_width x guest_height: any size with the guest's aspect ratio, to
// within the rounding of the pack's ripper (at most 3.5 texels per side: the
// upscaler's rounding, the 8192 cap and BC sides rounded to a multiple of 4),
// that is |width * guest_height - height * guest_width| <= 4 * (guest_width
// + guest_height).
bool IsReplacementSize(uint32_t guest_width, uint32_t guest_height, uint32_t width,
                       uint32_t height);

// How many mip levels a replacement image of width x height may have for a
// guest texture of guest_width x guest_height with guest_level_count levels:
// the guest's plus max(0, round(log2(scale))), scale being the ratio of the
// longest sides, so the image's smallest level is about the guest's smallest
// (the larger image's extra levels come on top of the guest's), and never
// more than a full chain of the image.
uint32_t GetReplacementLevelCount(uint32_t guest_width, uint32_t guest_height,
                                  uint32_t guest_level_count, uint32_t width, uint32_t height);

// The largest image side any DDS may have, whatever the GPU's limit, and the
// most mip levels a DDS header may declare (a full chain of that side).
constexpr uint32_t kMaxImageWidthHeight = 16384;
constexpr uint32_t kMaxImageLevels = 15;

// The DDS header's checked values, and what the file must look like, as
// ParseDdsHeader works them out from the 128 header bytes alone: nothing is
// allocated or read from the data until the caller has looked at them.
struct DdsInfo {
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  uint32_t width = 0;
  uint32_t height = 0;
  // The levels the header declares (1 if it declares none), at most
  // kMaxImageLevels and never more than a full chain.
  uint32_t level_count = 0;
  // The bytes of all declared levels: the file must be exactly 128 +
  // data_size bytes long.
  uint64_t data_size = 0;
  // For 32-bit texels: the byte of the file's texel each byte of the host
  // texel (B, G, R, A order, see the top of this file) comes from, and
  // whether the file has alpha (if not, the host texel gets opaque alpha).
  int bgra_source_bytes[4] = {0, 1, 2, 3};
  bool has_alpha = true;
};

// Checks a DDS file's first 128 bytes (header_size must be at least 128):
// magic, structure sizes, no DX10 header, no cube map or volume, one of the
// pack formats, sides 1 to kMaxImageWidthHeight, a sane mip count. Returns
// false with the reason in error_out.
bool ParseDdsHeader(const uint8_t* header, size_t header_size, DdsInfo& info_out,
                    std::string& error_out);

// Reads the replacement image for a guest texture of guest_width x
// guest_height in guest_format with guest_level_count mip levels: the DDS must
// be in that format, of a size IsReplacementSize accepts, with no side larger
// than max_width_height (the host's limit), and exactly as long as its header
// says (all checked before the data is read). Reads min(the file's levels,
// GetReplacementLevelCount) levels, no more than max_bytes of data (0 = no
// limit; the limit is for the pack's memory cap, see below). Returns false
// with the reason in error_out; `too_large_out` is set when only max_bytes
// stood in the way.
bool LoadDds(const std::filesystem::path& path, xenos::TextureFormat guest_format,
             uint32_t guest_width, uint32_t guest_height, uint32_t guest_level_count,
             uint32_t max_width_height, uint64_t max_bytes, Image& image_out,
             std::string& error_out, bool* too_large_out = nullptr);

// The memory cap for replacement textures (texture_replacement_memory_limit,
// in MB; 0 = none): the bytes of replacement images alive at once may not go
// beyond it; a texture that would is loaded from the guest data instead.
uint64_t GetMemoryLimitBytes();

// The alpha of an image level (rows of blocks without padding, as ComputeKey
// gathers a guest base level and LoadDds reads an image's levels):
// `opaque` if every texel's alpha is 255, the number of texels, how many are
// at least half opaque (alpha >= 128, what alpha testing keeps), and the sum
// of their alphas. k_DXN has no alpha (opaque).
struct AlphaStats {
  bool opaque = true;
  uint64_t texels = 0;
  uint64_t visible = 0;
  uint64_t alpha_sum = 0;
};
bool GetAlphaStats(xenos::TextureFormat format, uint32_t width, uint32_t height,
                   const uint8_t* data, size_t data_size, AlphaStats& stats_out);

// Makes every level of an image opaque: k_DXT1 blocks in the three-color mode
// draw their transparent texels with the middle color instead, k_DXT2_3 and
// k_DXT4_5 alpha becomes 255, k_8_8_8_8 alpha bytes 255. Returns how many
// blocks (or texels, for k_8_8_8_8) changed.
uint64_t ForceOpaque(Image& image);

// Keeps a replacement from being more see-through than the guest texture it
// replaces (so it can't open a view through a wall or a soldier's cover):
// if the guest's base level (`guest`: guest_width x guest_height in the
// image's format, as ComputeKey gathers it) is opaque, the image is made
// opaque (ForceOpaque); otherwise the image's top level must keep at least
// the guest's share of visible texels and its mean alpha, each to within 2%.
// Returns false, with the reason, when the image is refused; `forced_out`
// counts what ForceOpaque changed.
bool EnforceAlpha(uint32_t guest_width, uint32_t guest_height, const uint8_t* guest,
                  size_t guest_size, Image& image, uint64_t& forced_out, std::string& error_out);

}  // namespace rex::graphics::texture_replacement
