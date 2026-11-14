/**
 * @file        filesystem/xex.h
 * @brief       XEX image reader and PE image parsing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's kernel/util/xex_module.cc (Ben Vanik, 2020).
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @remarks     Parts derived from Xenia (Ben Vanik, 2013).
 */

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>
#include <rex/assert.h>
#include <rex/kernel/types/xex.h>

namespace rex::filesystem {

/** Characteristics bits of a PE section header. */
enum PESectionFlags : u32 {
  kPESectionContainsCode = 0x00000020,       /**< IMAGE_SCN_CNT_CODE. The section holds code. */
  kPESectionContainsDataInit = 0x00000040,   /**< IMAGE_SCN_CNT_INITIALIZED_DATA. */
  kPESectionContainsDataUninit = 0x00000080, /**< IMAGE_SCN_CNT_UNINITIALIZED_DATA. */
  kPESectionMemoryExecute = 0x20000000,      /**< IMAGE_SCN_MEM_EXECUTE. */
  kPESectionMemoryRead = 0x40000000,         /**< IMAGE_SCN_MEM_READ. The section is readable. */
  kPESectionMemoryWrite = 0x80000000,        /**< IMAGE_SCN_MEM_WRITE. */
};

class PESection {
 public:
  char name[9];    /**< Section name from the 8 byte PE name plus a terminator. */
  u32 raw_address; /**< File offset of the section data (PointerToRawData). */
  u32 raw_size;    /**< Size of the section data in the file (SizeOfRawData). */
  u32 address;     /**< Image relative address of the section when loaded (VirtualAddress). */
  u32 size;        /**< Size of the section in memory (VirtualSize). */
  u32 flags;       /**< kPESection* flag bits. */
};

struct PEExport {
  const char* name;
  u32 ordinal;

  u64 addr;
};

struct PEInfo {
  std::vector<PESection> sections; /**< Sections with `address` holding the RVA. */
  u32 exception_directory_rva = 0;
  u32 exception_directory_size = 0;
  u32 time_date_stamp = 0; /**< Linker timestamp in Unix seconds. */
  u32 header_size = 0;     /**< Header bytes, capped at the lowest section address. */
};

}  // namespace rex::filesystem

namespace rex::filesystem {

/** AES key that decrypts the image key of an XEX. */
enum class XEXKey : u8 {
  kRetail,
  kDevKit,
  kRetailXEX1, /**< Retail key of the XEX1 era. */
  kDevKitXEX1, /**< Devkit key of the XEX1 era. */
};

struct XEXPageRange {
  kernel::XEXSectionType type = kernel::kXEXSectionCode;
  u32 page_count = 0;
};

/** Security info of an XEX1 or XEX2 image. */
struct XEXSecurityInfo {
  std::array<u8, 0x100> rsa_signature{};
  std::array<u8, 0x10> aes_key{}; /**< Image key, encrypted with the retail or devkit key. */
  u32 image_size = 0;             /**< Image size the security info stores. */
  u32 image_flags = 0;            /**< `XEXImageFlags` bits. */
  u32 export_table = 0;           /**< Guest address of the export table, 0 for none. */
  u32 load_address = 0;
  std::vector<XEXPageRange> pages; /**< Page descriptors in image order. */
};

struct XEXImportLibrary {
  std::string name; /**< Library file name as the header stores it. */
  u32 id = 0;
  kernel::XEXVersion version{};
  kernel::XEXVersion min_version{};
  std::vector<u32> records; /**< Guest addresses of the import records. */
};

struct XEXOptHeaderEntry {
  u32 index = 0;
  u32 value = 0; /**< Value itself or an offset from the header start. */
};

/** Parsed XEX header, security info and import libraries. Create one with IFileSystemFactory. */
class IXEXImage {
 public:
  virtual ~IXEXImage() = default;

  /**
   * Returns the byte swapped XEX header.
   */
  virtual const kernel::XEXHeader& GetHeader() const = 0;

  virtual std::span<const u8> GetHeaderBytes() const = 0;

  /**
   * Returns the data of an optional header whose key low byte is not 0 or 1, or null when absent.
   */
  template <typename T>
  const T* GetOptHeader(kernel::XEXHeaderKey key) const {
    return static_cast<const T*>(FindOptHeader(key));
  }

  /**
   * Returns the value of an optional header whose key low byte is 0 or 1, or nothing when absent.
   */
  virtual std::optional<u32> GetOptHeaderValue(kernel::XEXHeaderKey key) const = 0;

  virtual const XEXSecurityInfo& GetSecurity() const = 0;

  virtual std::span<const XEXImportLibrary> GetImports() const = 0;

  virtual std::vector<u32> GetAlternateTitleIDs() const = 0;

  virtual bool IsPatch() const = 0;

  /** Returns the key Decode selected. Retail before a successful Decode. */
  virtual XEXKey GetKey() const = 0;

  /** Returns the image size from the page descriptors. */
  virtual u32 GetImageSize() const = 0;

  /**
   * Decrypts and decompresses the image, trying the keys in XEXKey order.
   *
   * @param file The whole file the image was parsed from.
   * @param image Receives the image. Bytes past the decoded data are 0.
   * @return Ok when a key yields a PE image, else the last error.
   */
  [[nodiscard]] virtual Result<void> Decode(std::span<const u8> file, std::span<u8> image) = 0;

  /**
   * Returns this image with the header delta of a patch applied.
   *
   * @param patch A patch image from the same factory.
   */
  [[nodiscard]] virtual Result<std::unique_ptr<IXEXImage>> ApplyHeaderDelta(
      const IXEXImage& patch) const = 0;

  /**
   * Applies the image delta of a patch to a decoded image.
   *
   * @param patch The patch image ApplyHeaderDelta used.
   * @param image The decoded image, sized for the larger of the old and new images.
   * @return Ok, or a Crypto or Compression error.
   */
  [[nodiscard]] virtual Result<void> ApplyImageDelta(const IXEXImage& patch,
                                                     std::span<u8> image) const = 0;

 protected:
  virtual const void* FindOptHeader(kernel::XEXHeaderKey key) const = 0;
};

}  // namespace rex::filesystem
