#include <rex/filesystem/xex.h>

#include <algorithm>
#include <cstring>

#include <rex/logging.h>

#include "crypto/TinySHA1.hpp"
#include "crypto/rijndael-alg-fst.h"
#include "lzx.h"
#include "pe/pe_image.h"

namespace rex::filesystem {

namespace {

using Key = std::array<u8, 16>;

constexpr u32 kXEX1Magic = 0x58455831;
constexpr u32 kXEX2Magic = 0x58455832;
constexpr u32 kExecutableMagic = 0x00905A4D;
constexpr size_t kHeaderTableOffset = 0x18;
constexpr size_t kXEX1PageTableOffset = 0x168;
constexpr size_t kXEX2PageTableOffset = 0x184;
constexpr size_t kBlockInfoSize = 24;

constexpr std::array<Key, 4> kMasterKeys = {{
    {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF,
     0x91},
    {},
    {0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9, 0x8B, 0x99, 0x92, 0x2C, 0xE9, 0x32, 0x15,
     0x72},
    {0xA8, 0xB0, 0x05, 0x12, 0xED, 0xE3, 0x63, 0x8D, 0xC6, 0x58, 0xB3, 0x10, 0x1F, 0x9F, 0x50,
     0xD1},
}};

const Key& MasterKey(XEXKey key) {
  return kMasterKeys[static_cast<size_t>(key)];
}

class CBCDecryptor {
 public:
  explicit CBCDecryptor(const Key& key) {
    rounds_ = rijndaelKeySetupDec(round_keys_, key.data(), 128);
  }

  void Run(std::span<const u8> input, std::span<u8> output) {
    for (size_t offset = 0; offset + 16 <= input.size(); offset += 16) {
      std::array<u8, 16> block{};
      rijndaelDecrypt(round_keys_, rounds_, input.data() + offset, block.data());
      for (size_t i = 0; i < 16; ++i) {
        output[offset + i] = block[i] ^ chain_[i];
        chain_[i] = input[offset + i];
      }
    }
  }

 private:
  u32 round_keys_[4 * (MAXNR + 1)]{};
  int rounds_ = 0;
  std::array<u8, 16> chain_{};
};

Key DecryptKey(const Key& key, std::span<const u8, 16> encrypted) {
  Key out{};
  CBCDecryptor(key).Run(encrypted, out);
  return out;
}

std::array<u8, 20> SHA1Digest(std::span<const u8> data) {
  sha1::SHA1 sha;
  sha.processBytes(data.data(), data.size());
  std::array<u8, 20> digest{};
  sha.finalize(digest.data());
  return digest;
}

bool IsExecutable(std::span<const u8> image) {
  if (image.size() < 4) {
    return false;
  }
  u32 magic = 0;
  std::memcpy(&magic, image.data(), 4);
  return magic == kExecutableMagic;
}

template <typename T>
Result<T> FormatError(std::string message) {
  return Err<T>(ErrorCategory::Format, std::move(message));
}

}  // namespace

std::optional<XEXOptHeaderEntry> FindOptHeaderEntry(std::span<const u8> header,
                                                    xex2_header_keys key) {
  if (header.size() < kHeaderTableOffset) {
    return std::nullopt;
  }
  const auto* parsed = reinterpret_cast<const xex2_header*>(header.data());
  const u32 count = parsed->header_count;
  if (kHeaderTableOffset + u64{count} * sizeof(xex2_opt_header) > header.size()) {
    return std::nullopt;
  }
  for (u32 i = 0; i < count; ++i) {
    if (parsed->headers[i].key == key) {
      return XEXOptHeaderEntry{i, parsed->headers[i].value};
    }
  }
  return std::nullopt;
}

Result<XEXImage> XEXImage::Parse(std::span<const u8> file) {
  if (file.size() < kHeaderTableOffset) {
    return FormatError<XEXImage>("XEX file is smaller than its header");
  }
  const auto* header = reinterpret_cast<const xex2_header*>(file.data());
  const u32 magic = header->magic;
  if (magic != kXEX1Magic && magic != kXEX2Magic) {
    return FormatError<XEXImage>("Not an XEX file");
  }
  const u32 header_size = header->header_size;
  if (header_size < kHeaderTableOffset || header_size > file.size()) {
    return FormatError<XEXImage>("XEX header size is out of range");
  }
  XEXImage image;
  image.xex1_ = magic == kXEX1Magic;
  image.header_.assign(file.begin(), file.begin() + header_size);
  REX_TRY_VOID(image.ReadHeader());
  if (image.is_patch()) {
    image.patch_data_.assign(file.begin() + header_size, file.end());
  }
  return image;
}

const xex2_header& XEXImage::header() const {
  return *reinterpret_cast<const xex2_header*>(header_.data());
}

std::span<const u8> XEXImage::header_bytes() const {
  return header_;
}

const void* XEXImage::FindOptHeader(xex2_header_keys key) const {
  const auto entry = FindOptHeaderEntry(header_, key);
  if (!entry) {
    return nullptr;
  }
  switch (key & 0xFF) {
    case 0x00:
      return nullptr;
    case 0x01:
      return &header().headers[entry->index].value;
    default:
      return entry->value < header_.size() ? header_.data() + entry->value : nullptr;
  }
}

std::optional<u32> XEXImage::OptHeaderValue(xex2_header_keys key) const {
  if ((key & 0xFF) > 0x01) {
    return std::nullopt;
  }
  const auto entry = FindOptHeaderEntry(header_, key);
  if (!entry) {
    return std::nullopt;
  }
  return entry->value;
}

const XEXSecurityInfo& XEXImage::security() const {
  return security_;
}

std::span<const XEXImportLibrary> XEXImage::imports() const {
  return imports_;
}

std::vector<u32> XEXImage::alternate_title_ids() const {
  std::vector<u32> ids;
  const auto* list = OptHeader<xex2_opt_generic_u32>(XEX_HEADER_ALTERNATE_TITLE_IDS);
  if (!list) {
    return ids;
  }
  const u32 count = list->count();
  const auto* end = header_.data() + header_.size();
  for (u32 i = 0; i < count; ++i) {
    if (reinterpret_cast<const u8*>(&list->values[i + 1]) > end) {
      break;
    }
    ids.push_back(list->values[i]);
  }
  return ids;
}

bool XEXImage::is_patch() const {
  return (static_cast<u32>(header().module_flags) &
          (XEX_MODULE_MODULE_PATCH | XEX_MODULE_PATCH_DELTA | XEX_MODULE_PATCH_FULL)) != 0;
}

XEXKey XEXImage::key() const {
  return key_;
}

u32 XEXImage::image_size() const {
  const u32 page_size = (security_.image_flags & XEX_IMAGE_PAGE_SIZE_4KB) ? 0x1000 : 0x10000;
  u32 total = 0;
  for (const auto& range : security_.pages) {
    total += range.page_count * page_size;
  }
  return total;
}

Result<void> XEXImage::ReadHeader() {
  const u32 count = header().header_count;
  if (kHeaderTableOffset + u64{count} * sizeof(xex2_opt_header) > header_.size()) {
    return FormatError<void>("XEX optional header table is out of range");
  }
  REX_TRY_VOID(ReadSecurityInfo());
  return ReadImports();
}

Result<void> XEXImage::ReadSecurityInfo() {
  const u64 offset = header().security_offset;
  const size_t table = xex1_ ? kXEX1PageTableOffset : kXEX2PageTableOffset;
  if (offset + table > header_.size()) {
    return FormatError<void>("XEX security info is out of range");
  }
  auto read = [&](const auto& info) -> Result<void> {
    std::memcpy(security_.rsa_signature.data(), info.rsa_signature, 0x100);
    std::memcpy(security_.aes_key.data(), info.aes_key, 0x10);
    security_.image_size = info.image_size;
    security_.image_flags = info.image_flags;
    security_.export_table = info.export_table;
    security_.load_address = info.load_address;
    const u32 count = info.page_descriptor_count;
    if (offset + table + u64{count} * sizeof(xex2_page_descriptor) > header_.size()) {
      return FormatError<void>("XEX page descriptors are out of range");
    }
    security_.pages.clear();
    for (u32 i = 0; i < count; ++i) {
      const u32 value = info.page_descriptors[i].value;
      security_.pages.push_back({static_cast<xex2_section_type>(value & 0xF), value >> 4});
    }
    return Ok();
  };
  const u8* base = header_.data() + offset;
  if (xex1_) {
    return read(*reinterpret_cast<const xex1_security_info*>(base));
  }
  return read(*reinterpret_cast<const xex2_security_info*>(base));
}

Result<void> XEXImage::ReadImports() {
  imports_.clear();
  const auto* libraries = OptHeader<xex2_opt_import_libraries>(XEX_HEADER_IMPORT_LIBRARIES);
  if (!libraries) {
    return Ok();
  }
  const auto* base = reinterpret_cast<const u8*>(libraries);
  const u32 total = libraries->size;
  if (base + total > header_.data() + header_.size() || total < 12) {
    return FormatError<void>("XEX import libraries are out of range");
  }
  const u32 table_size = libraries->string_table.size;
  const u32 table_count = libraries->string_table.count;
  if (u64{table_size} + 12 > total) {
    return FormatError<void>("XEX import string table is out of range");
  }
  std::vector<std::string_view> names;
  for (size_t i = 0; i < table_size && names.size() < table_count;) {
    const char* text = &libraries->string_table.data[i];
    const size_t length = strnlen(text, table_size - i);
    if (length && static_cast<u8>(text[0]) != 0xCD) {
      names.emplace_back(text, length);
    }
    i += std::max<size_t>(length, 1);
    if (i % 4) {
      i += 4 - i % 4;
    }
  }
  constexpr u32 kLibraryHeaderSize = 0x28;
  u32 offset = table_size + 12;
  for (u32 n = 0; n < table_count; ++n) {
    if (u64{offset} + kLibraryHeaderSize > total) {
      return FormatError<void>("XEX import library is out of range");
    }
    const auto* library = reinterpret_cast<const xex2_import_library*>(base + offset);
    const u32 size = library->size;
    const u32 count = library->count;
    if (size < kLibraryHeaderSize || u64{offset} + size > total ||
        kLibraryHeaderSize + u64{count} * 4 > size) {
      return FormatError<void>("XEX import library is out of range");
    }
    const u32 name_index = library->name_index & 0xFF;
    if (name_index >= names.size()) {
      return FormatError<void>("XEX import library names a missing string");
    }
    XEXImportLibrary out;
    out.name = std::string(names[name_index]);
    out.id = library->id;
    out.version.value = library->version().value;
    out.min_version.value = library->version_min().value;
    out.records.reserve(count);
    for (u32 i = 0; i < count; ++i) {
      out.records.push_back(library->import_table[i]);
    }
    imports_.push_back(std::move(out));
    offset += size;
  }
  return Ok();
}

Result<void> XEXImage::Decode(std::span<const u8> file, std::span<u8> image) {
  if (is_patch()) {
    return FormatError<void>("An XEX patch has no image to decode");
  }
  const auto* format = OptHeader<xex2_opt_file_format_info>(XEX_HEADER_FILE_FORMAT_INFO);
  if (!format) {
    return FormatError<void>("XEX has no file format header");
  }
  if (file.size() < header_.size()) {
    return FormatError<void>("XEX file is smaller than its header");
  }
  const auto data = file.subspan(header_.size());
  Error last(ErrorCategory::Format, "XEX image is not a PE image");
  for (const auto key : {XEXKey::Retail, XEXKey::DevKit, XEXKey::RetailXEX1, XEXKey::DevKitXEX1}) {
    std::ranges::fill(image, u8{0});
    session_key_ = DecryptKey(MasterKey(key), security_.aes_key);
    auto decoded = DecodeWith(*format, data, image);
    if (decoded && IsExecutable(image)) {
      key_ = key;
      return Ok();
    }
    last = decoded ? Error(ErrorCategory::Format, "XEX image is not a PE image") : decoded.error();
  }
  return Err(std::move(last));
}

Result<void> XEXImage::DecodeWith(const xex2_opt_file_format_info& format, std::span<const u8> data,
                                  std::span<u8> image) const {
  const u16 encryption = format.encryption_type;
  if (encryption != XEX_ENCRYPTION_NONE && encryption != XEX_ENCRYPTION_NORMAL) {
    return Err(ErrorCategory::Crypto, "Unknown XEX encryption type");
  }
  const bool encrypted = encryption == XEX_ENCRYPTION_NORMAL;
  switch (static_cast<u16>(format.compression_type)) {
    case XEX_COMPRESSION_NONE: {
      if (data.size() > image.size()) {
        return FormatError<void>("XEX image data is larger than the image");
      }
      if (encrypted) {
        CBCDecryptor(session_key_).Run(data, image);
      } else {
        std::ranges::copy(data, image.begin());
      }
      return Ok();
    }
    case XEX_COMPRESSION_BASIC: {
      const u32 block_count = (format.info_size - 8) / 8;
      const auto* blocks = format.compression_info.basic.blocks;
      if (reinterpret_cast<const u8*>(blocks + block_count) > header_.data() + header_.size()) {
        return FormatError<void>("XEX basic compression table is out of range");
      }
      CBCDecryptor decryptor(session_key_);
      size_t in = 0;
      size_t out = 0;
      for (u32 n = 0; n < block_count; ++n) {
        const u32 data_size = blocks[n].data_size;
        const u32 zero_size = blocks[n].zero_size;
        if (in + data_size > data.size() || out + u64{data_size} + zero_size > image.size()) {
          return FormatError<void>("XEX basic compression block is out of range");
        }
        if (encrypted) {
          decryptor.Run(data.subspan(in, data_size), image.subspan(out, data_size));
        } else {
          std::ranges::copy(data.subspan(in, data_size), image.begin() + out);
        }
        in += data_size;
        out += data_size + zero_size;
      }
      return Ok();
    }
    case XEX_COMPRESSION_NORMAL: {
      std::vector<u8> plain;
      std::span<const u8> input = data;
      if (encrypted) {
        plain.resize(data.size());
        CBCDecryptor(session_key_).Run(data, plain);
        input = plain;
      }
      const auto& normal = format.compression_info.normal;
      std::vector<u8> compressed;
      compressed.reserve(input.size());
      u32 block_size = normal.first_block.block_size;
      std::array<u8, 20> expected{};
      std::memcpy(expected.data(), normal.first_block.block_hash, 20);
      size_t offset = 0;
      while (block_size) {
        if (block_size < kBlockInfoSize || offset + block_size > input.size()) {
          return FormatError<void>("XEX compressed block is out of range");
        }
        const auto block = input.subspan(offset, block_size);
        if (SHA1Digest(block) != expected) {
          return Err(ErrorCategory::Crypto, "XEX compressed block hash does not match");
        }
        const auto* next = reinterpret_cast<const xex2_compressed_block_info*>(block.data());
        const u32 next_size = next->block_size;
        std::memcpy(expected.data(), next->block_hash, 20);
        size_t p = kBlockInfoSize;
        while (true) {
          if (p + 2 > block.size()) {
            return FormatError<void>("XEX compressed chunk is out of range");
          }
          const size_t chunk = (size_t{block[p]} << 8) | block[p + 1];
          p += 2;
          if (!chunk) {
            break;
          }
          if (p + chunk > block.size()) {
            return FormatError<void>("XEX compressed chunk is out of range");
          }
          compressed.insert(compressed.end(), block.begin() + p, block.begin() + p + chunk);
          p += chunk;
        }
        offset += block_size;
        block_size = next_size;
      }
      if (const int code = detail::LZXDecompress(compressed, image, normal.window_size, {})) {
        return Err(ErrorCategory::Compression, "LZX decompression of the XEX image failed", code);
      }
      return Ok();
    }
    default:
      return FormatError<void>("Unknown XEX compression type");
  }
}

XEXImage XEXImage::Clone() const {
  XEXImage copy;
  copy.header_ = header_;
  copy.patch_data_ = patch_data_;
  copy.security_ = security_;
  copy.imports_ = imports_;
  copy.session_key_ = session_key_;
  copy.previous_session_key_ = previous_session_key_;
  copy.xex1_ = xex1_;
  copy.key_ = key_;
  return copy;
}

Result<XEXImage> XEXImage::ApplyHeaderDelta(const XEXImage& patch) const {
  if (!patch.is_patch()) {
    return FormatError<XEXImage>("XEX is not a patch");
  }
  const auto* descriptor =
      patch.OptHeader<xex2_opt_delta_patch_descriptor>(XEX_HEADER_DELTA_PATCH_DESCRIPTOR);
  const auto* format = patch.OptHeader<xex2_opt_file_format_info>(XEX_HEADER_FILE_FORMAT_INFO);
  if (!descriptor || !format) {
    return FormatError<XEXImage>("XEX patch has no delta descriptor");
  }
  if (std::memcmp(SHA1Digest(security_.rsa_signature).data(), descriptor->digest_source, 20) != 0) {
    REXLOG_WARN(filesystem, "XEX patch source hash does not match the base XEX signature hash");
  }
  const u32 size = static_cast<u32>(header_.size());
  const u32 source_offset = descriptor->delta_headers_source_offset;
  const u32 source_size = descriptor->delta_headers_source_size;
  const u32 target_offset = descriptor->delta_headers_target_offset;
  const u32 target_headers = descriptor->size_of_target_headers;
  if (source_offset > size) {
    return FormatError<XEXImage>("XEX header patch source is outside the base XEX header");
  }
  if (source_size > size - source_offset) {
    return FormatError<XEXImage>("XEX header patch source is too large");
  }
  if (target_offset > target_headers) {
    return FormatError<XEXImage>("XEX header patch target is outside the base XEX header");
  }
  if (source_size > target_headers - target_offset) {
    return FormatError<XEXImage>("XEX header patch target is too small");
  }
  const u32 target_size = target_headers ? target_headers : target_offset + source_size;

  XEXImage out = Clone();
  if (target_size > out.header_.size()) {
    out.header_.resize(target_size);
  }
  if (source_offset) {
    std::memmove(out.header_.data() + target_offset, out.header_.data() + source_offset,
                 source_size);
  }
  if (target_size < out.header_.size()) {
    std::fill(out.header_.begin() + target_size, out.header_.end(), u8{0});
  }
  const auto* info = reinterpret_cast<const u8*>(&descriptor->info);
  const size_t info_size = size_t{descriptor->info.compressed_len} + 12;
  if (info + info_size > patch.header_.data() + patch.header_.size()) {
    return FormatError<XEXImage>("XEX header patch is out of range");
  }
  if (const int code = detail::LZXApplyDelta(
          {info, info_size}, format->compression_info.normal.window_size, out.header_)) {
    return Err<XEXImage>(ErrorCategory::Compression, "XEX header patch failed", code);
  }
  if (out.header_.size() > target_size) {
    out.header_.resize(target_size);
  }
  REX_TRY_VOID(out.ReadHeader());
  out.previous_session_key_ = session_key_;
  out.session_key_ = DecryptKey(MasterKey(key_), out.security_.aes_key);
  return out;
}

Result<void> XEXImage::ApplyImageDelta(const XEXImage& patch, std::span<u8> image) const {
  const auto* descriptor =
      patch.OptHeader<xex2_opt_delta_patch_descriptor>(XEX_HEADER_DELTA_PATCH_DESCRIPTOR);
  const auto* format = patch.OptHeader<xex2_opt_file_format_info>(XEX_HEADER_FILE_FORMAT_INFO);
  if (!descriptor || !format) {
    return FormatError<void>("XEX patch has no delta descriptor");
  }
  const Key patch_key = DecryptKey(session_key_, patch.security_.aes_key);
  if (DecryptKey(session_key_, std::span<const u8, 16>(descriptor->image_key_source, 16)) !=
      previous_session_key_) {
    return Err(ErrorCategory::Crypto, "XEX patch image key does not match the base XEX");
  }

  std::vector<u8> plain;
  std::span<const u8> input = patch.patch_data_;
  const u16 encryption = format->encryption_type;
  if (encryption == XEX_ENCRYPTION_NORMAL) {
    plain.resize(input.size());
    CBCDecryptor(patch_key).Run(input, plain);
    input = plain;
  } else if (encryption != XEX_ENCRYPTION_NONE) {
    return Err(ErrorCategory::Crypto, "Unknown XEX patch encryption type");
  }

  const u32 source_offset = descriptor->delta_image_source_offset;
  const u32 source_size = descriptor->delta_image_source_size;
  const u32 target_offset = descriptor->delta_image_target_offset;
  if (source_offset) {
    if (u64{source_offset} + source_size > image.size() ||
        u64{target_offset} + source_size > image.size()) {
      return FormatError<void>("XEX image patch source is out of range");
    }
    std::memmove(image.data() + target_offset, image.data() + source_offset, source_size);
  }
  const u64 target_end = u64{target_offset} + source_size;
  if (target_end < image.size()) {
    std::fill(image.begin() + static_cast<ptrdiff_t>(target_end), image.end(), u8{0});
  }

  u32 block_size = format->compression_info.normal.first_block.block_size;
  std::array<u8, 20> expected{};
  std::memcpy(expected.data(), format->compression_info.normal.first_block.block_hash, 20);
  size_t offset = 0;
  while (block_size) {
    if (block_size < kBlockInfoSize || offset + block_size > input.size()) {
      return FormatError<void>("XEX patch block is out of range");
    }
    const auto block = input.subspan(offset, block_size);
    if (SHA1Digest(block) != expected) {
      return Err(ErrorCategory::Crypto, "XEX patch block hash does not match");
    }
    const auto* next = reinterpret_cast<const xex2_compressed_block_info*>(block.data());
    const u32 next_size = next->block_size;
    std::memcpy(expected.data(), next->block_hash, 20);
    if (const int code = detail::LZXApplyDelta(
            block.subspan(kBlockInfoSize), format->compression_info.normal.window_size, image)) {
      return Err(ErrorCategory::Compression, "XEX image patch failed", code);
    }
    offset += block_size;
    block_size = next_size;
  }
  return Ok();
}

Result<PEInfo> ParsePE(std::span<const u8> image) {
  if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
    return FormatError<PEInfo>("PE image is smaller than its DOS header");
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return FormatError<PEInfo>("PE signature mismatch, the image was decoded with the wrong key");
  }
  const size_t nt_offset = static_cast<u32>(dos->e_lfanew);
  if (nt_offset + sizeof(IMAGE_NT_HEADERS32) > image.size()) {
    return FormatError<PEInfo>("PE NT headers are out of range");
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + nt_offset);
  const auto& file = nt->FileHeader;
  const auto& optional = nt->OptionalHeader;
  if (nt->Signature != IMAGE_NT_SIGNATURE || file.Machine != IMAGE_FILE_MACHINE_POWERPCBE ||
      !(file.Characteristics & IMAGE_FILE_32BIT_MACHINE) ||
      file.SizeOfOptionalHeader != IMAGE_SIZEOF_NT_OPTIONAL_HEADER ||
      optional.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      optional.Subsystem != IMAGE_SUBSYSTEM_XBOX) {
    return FormatError<PEInfo>("Not a big endian PowerPC PE image");
  }
  const size_t sections_offset =
      nt_offset + 4 + sizeof(IMAGE_FILE_HEADER) + file.SizeOfOptionalHeader;
  if (sections_offset + size_t{file.NumberOfSections} * sizeof(IMAGE_SECTION_HEADER) >
      image.size()) {
    return FormatError<PEInfo>("PE section table is out of range");
  }
  PEInfo info;
  info.time_date_stamp = file.TimeDateStamp;
  info.exception_directory_rva =
      optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
  info.exception_directory_size = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size;
  const auto* sections =
      reinterpret_cast<const IMAGE_SECTION_HEADER*>(image.data() + sections_offset);
  for (u16 n = 0; n < file.NumberOfSections; ++n) {
    PESection section{};
    std::memcpy(section.name, sections[n].Name, 8);
    section.name[8] = 0;
    section.raw_address = sections[n].PointerToRawData;
    section.raw_size = sections[n].SizeOfRawData;
    section.address = sections[n].VirtualAddress;
    section.size = sections[n].Misc.VirtualSize;
    section.flags = sections[n].Characteristics;
    info.sections.push_back(section);
  }
  return info;
}

}  // namespace rex::filesystem
