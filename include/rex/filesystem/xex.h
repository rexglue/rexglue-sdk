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

#include <rex/platform.h>
#include <rex/filesystem/xex_types.h>
#include <rex/export.h>
#include <rex/result.h>
#include <rex/types.h>

namespace rex::filesystem {

/** AES key that decrypts the image key of an XEX. */
enum class XEXKey : u8 {
  kRetail,     /**< Retail console key. */
  kDevKit,     /**< Devkit key. */
  kRetailXEX1, /**< Retail key of the XEX1 era. */
  kDevKitXEX1, /**< Devkit key of the XEX1 era. */
};

/** Section kind and page count of an XEX page descriptor. */
struct XEXPageRange {
  XEXSectionType type = kXEXSectionCode; /**< Code, data or read only data. */
  u32 page_count = 0;                    /**< Number of pages the range covers. */
};

/** Security info of an XEX1 or XEX2 image in a layout. */
struct XEXSecurityInfo {
  std::array<u8, 0x100> rsa_signature{}; /**< RSA signature of the image. */
  std::array<u8, 0x10> aes_key{};        /**< Image key, encrypted with the retail or devkit key. */
  u32 image_size = 0;                    /**< Image size the security info stores. */
  u32 image_flags = 0;                   /**< `XEXImageFlags` bits. */
  u32 export_table = 0;                  /**< Guest address of the export table, 0 for none. */
  u32 load_address = 0;                  /**< Guest address the image loads at. */
  std::vector<XEXPageRange> pages;       /**< Page descriptors in image order. */
};

/** Import library entry of an XEX header. */
struct XEXImportLibrary {
  std::string name;         /**< Library file name as the header stores it. */
  u32 id = 0;               /**< Library ID. */
  XEXVersion version{};     /**< Library version. */
  XEXVersion min_version{}; /**< Minimum library version. */
  std::vector<u32> records; /**< Guest addresses of the import records. */
};

/** Optional header table entry. */
struct XEXOptHeaderEntry {
  u32 index = 0; /**< Index of the entry in the header table. */
  u32 value = 0; /**< Value itself or an offset from the header start. */
};

/** Sections, exception directory and timestamp of a decoded PE image. */
struct PEInfo {
  std::vector<PESection> sections;  /**< Sections with `address` holding the RVA. */
  u32 exception_directory_rva = 0;  /**< RVA of the exception directory. */
  u32 exception_directory_size = 0; /**< Size of the exception directory. */
  u32 time_date_stamp = 0;          /**< Linker timestamp in Unix seconds. */
  u32 header_size = 0;              /**< Header bytes, capped at the lowest section address. */
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
   * Returns the XEX header.
   *
   * @return The byte swapped XEX header at the start of the file.
   */
  const XEXHeader& GetHeader() const;

  /**
   * Returns the header bytes.
   *
   * @return `header_size` bytes.
   */
  std::span<const u8> GetHeaderBytes() const;

  /**
   * Returns the data of an optional header whose key low byte is not 0 or 1.
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
  const XEXSecurityInfo& GetSecurity() const;

  /**
   * Returns the import libraries.
   *
   * @return The libraries in header order.
   */
  std::span<const XEXImportLibrary> GetImports() const;

  /**
   * Returns the alternate title IDs of the header.
   *
   * @return The IDs, empty when the header has none.
   */
  std::vector<u32> GetAlternateTitleIds() const;

  /**
   * Reports whether the image is a delta or full patch.
   *
   * @return True when the module flags mark a patch.
   */
  bool IsPatch() const;

  /**
   * Returns the key Decode selected.
   *
   * @return The key, Retail before a successful Decode.
   */
  XEXKey GetKey() const;

  /**
   * Returns the image size the page descriptors describe with the page size the image flags name.
   *
   * @return The size in bytes.
   */
  u32 GetImageSize() const;

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
   * Applies the image delta of a patch to a decoded image.
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
  XEXKey key_ = XEXKey::kRetail;
};

/**
 * Parses the PE headers of a decoded image.
 *
 * @param image The decoded image.
 * @return The PE info, or a Format error when the image is not a big endian PowerPC PE image.
 */
[[nodiscard]] REX_API Result<PEInfo> ParsePE(std::span<const u8> image);

}  // namespace rex::filesystem
