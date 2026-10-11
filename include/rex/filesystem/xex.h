/**
 * @file        filesystem/xex.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's kernel/util/xex_module.cc (Ben Vanik, 2020).
 */

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/xex_types.h>
#include <rex/macros.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

/** AES key that decrypts the image key of an XEX. */
enum class XEXKey : u8 {
  /** Retail console key. */
  Retail,
  /** Devkit key. */
  DevKit,
  /** Retail key of the XEX1 era. */
  RetailXEX1,
  /** Devkit key of the XEX1 era. */
  DevKitXEX1,
};

/** Section kind and page count of 1 XEX page descriptor. */
struct XEXPageRange {
  /** Code, data or read only data. */
  XEXSectionType type = XEX_SECTION_CODE;
  /** Number of pages the range covers. */
  u32 page_count = 0;
};

/** Security info of an XEX1 or XEX2 image in 1 layout. */
struct XEXSecurityInfo {
  /** RSA signature of the image. */
  std::array<u8, 0x100> rsa_signature{};
  /** Image key, encrypted with the retail or devkit key. */
  std::array<u8, 0x10> aes_key{};
  /** Image size the security info stores. */
  u32 image_size = 0;
  /** `XEXImageFlags` bits. */
  u32 image_flags = 0;
  /** Guest address of the export table, 0 for none. */
  u32 export_table = 0;
  /** Guest address the image loads at. */
  u32 load_address = 0;
  /** Page descriptors in image order. */
  std::vector<XEXPageRange> pages;
};

/** Import library entry of an XEX header. */
struct XEXImportLibrary {
  /** Library file name as the header stores it. */
  std::string name;
  /** Library ID. */
  u32 id = 0;
  /** Library version. */
  XEXVersion version{};
  /** Minimum library version. */
  XEXVersion min_version{};
  /** Guest addresses of the import records. */
  std::vector<u32> records;
};

/** Optional header table entry. */
struct XEXOptHeaderEntry {
  /** Index of the entry in the header table. */
  u32 index = 0;
  /** Value field of the entry, the value itself or an offset from the header start. */
  u32 value = 0;
};

/** Sections, exception directory and timestamp of a decoded PE image. */
struct PEInfo {
  /** Sections with `address` holding the RVA. */
  std::vector<PESection> sections;
  /** RVA of the exception directory. */
  u32 exception_directory_rva = 0;
  /** Size of the exception directory. */
  u32 exception_directory_size = 0;
  /** Linker timestamp in Unix seconds. */
  u32 time_date_stamp = 0;
};

/**
 * Finds an optional header entry in a raw XEX header.
 *
 * @param header The header bytes, at least `header_size` long.
 * @param key The header key.
 * @return The entry, or nothing when the key is absent or the table is out of range.
 */
REX_API std::optional<XEXOptHeaderEntry> FindOptHeaderEntry(std::span<const u8> header,
                                                            XEXHeaderKey key);

/** Parsed XEX header, security info and import libraries. */
class REX_API XEXImage : public MoveOnly {
 public:
  XEXImage() = default;

  /**
   * Parses the header of an XEX1 or XEX2 file.
   *
   * @param file The whole file.
   * @return The image, or a Format error for a bad magic, a truncated file or a header table out
   *         of range.
   */
  [[nodiscard]] static Result<XEXImage> Parse(std::span<const u8> file);

  /**
   * Returns the XEX header. Valid after Parse.
   *
   * @return The byte swapped XEX header at the start of the file.
   */
  const XEXHeader& header() const;

  /**
   * Returns the header bytes.
   *
   * @return `header_size` bytes.
   */
  std::span<const u8> header_bytes() const;

  /**
   * Returns the data of an optional header whose key low byte is not 0.
   *
   * @tparam T The header structure.
   * @param key The header key.
   * @return The data, or null when absent or out of range.
   */
  template <typename T>
  const T* OptHeader(XEXHeaderKey key) const {
    return static_cast<const T*>(FindOptHeader(key));
  }

  /**
   * Returns the value of an optional header whose key low byte is 0 or 1.
   *
   * @param key The header key.
   * @return The value, or nothing when absent.
   */
  std::optional<u32> OptHeaderValue(XEXHeaderKey key) const;

  /**
   * Returns the security info.
   *
   * @return The security info.
   */
  const XEXSecurityInfo& security() const;

  /**
   * Returns the import libraries.
   *
   * @return The libraries in header order.
   */
  std::span<const XEXImportLibrary> imports() const;

  /**
   * Returns the alternate title IDs of the header.
   *
   * @return The IDs, empty when the header has none.
   */
  std::vector<u32> alternate_title_ids() const;

  /**
   * Reports whether the image is a delta or full patch.
   *
   * @return True when the module flags mark a patch.
   */
  bool is_patch() const;

  /**
   * Returns the key Decode selected.
   *
   * @return The key, Retail before a successful Decode.
   */
  XEXKey key() const;

  /**
   * Returns the image size the page descriptors describe with the page size the image flags name.
   *
   * @return The size in bytes.
   */
  u32 image_size() const;

  /**
   * Decrypts and decompresses the image, trying the keys in XEXKey order.
   *
   * @param file The whole file Parse read.
   * @param image The buffer that receives the image. Bytes past the decoded data are 0.
   * @return Ok when a key yields a PE image, otherwise the error of the last attempt.
   */
  [[nodiscard]] Result<void> Decode(std::span<const u8> file, std::span<u8> image);

  /**
   * Returns this image with the header delta of a patch applied.
   *
   * @param patch A patch image.
   * @return The patched image, which keeps this image's key for ApplyImageDelta.
   */
  [[nodiscard]] Result<XEXImage> ApplyHeaderDelta(const XEXImage& patch) const;

  /**
   * Applies the image delta of a patch to a decoded image. Called on the result of
   * ApplyHeaderDelta.
   *
   * @param patch The patch image ApplyHeaderDelta used.
   * @param image The decoded image, as large as the larger of the old and new image sizes.
   * @return Ok, or a Crypto error for a key or hash mismatch, or a Compression error.
   */
  [[nodiscard]] Result<void> ApplyImageDelta(const XEXImage& patch, std::span<u8> image) const;

 private:
  const void* FindOptHeader(XEXHeaderKey key) const;
  [[nodiscard]] Result<void> ReadHeader();
  [[nodiscard]] Result<void> ReadSecurityInfo();
  [[nodiscard]] Result<void> ReadImports();
  [[nodiscard]] Result<void> DecodeWith(const XEXOptFileFormatInfo& format,
                                        std::span<const u8> data, std::span<u8> image) const;
  XEXImage Clone() const;

  std::vector<u8> header_;
  std::vector<u8> patch_data_;
  XEXSecurityInfo security_;
  std::vector<XEXImportLibrary> imports_;
  std::array<u8, 16> session_key_{};
  std::array<u8, 16> previous_session_key_{};
  bool xex1_ = false;
  XEXKey key_ = XEXKey::Retail;
};

/**
 * Parses the PE headers of a decoded image.
 *
 * @param image The decoded image.
 * @return The PE info, or a Format error when the image is not a big endian PowerPC PE image.
 */
[[nodiscard]] REX_API Result<PEInfo> ParsePE(std::span<const u8> image);

}  // namespace rex::filesystem
