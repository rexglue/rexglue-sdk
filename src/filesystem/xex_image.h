/**
 * @file        filesystem/xex_image.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <rex/filesystem/xex.h>
#include <rex/kernel/types/xex.h>
#include <rex/platform.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex::filesystem {

/**
 * Finds an optional header entry in a raw XEX header.
 *
 * @param header The header bytes, at least `header_size` long.
 * @return The entry, or nothing when the key is absent or the table is out of range.
 */
std::optional<XEXOptHeaderEntry> FindOptHeaderEntry(std::span<const u8> header,
                                                    kernel::XEXHeaderKey key);

/**
 * Parses the PE headers of a decoded image.
 *
 * @return The PE info, or a Format error when the image is not a big endian PowerPC PE image.
 */
[[nodiscard]] Result<PEInfo> ParsePE(std::span<const u8> image);

/** The XEX header, security info and import libraries of one file. */
class XEXImage final : public IXEXImage, public MoveOnly {
 public:
  XEXImage() = default;

  /**
   * Parses the header of an XEX1 or XEX2 file.
   *
   * @return The image, or a Format error for a bad magic, a truncated file or a header table out
   *         of range.
   */
  [[nodiscard]] static Result<XEXImage> Parse(std::span<const u8> file);

  const kernel::XEXHeader& GetHeader() const override;
  std::span<const u8> GetHeaderBytes() const override;
  std::optional<u32> GetOptHeaderValue(kernel::XEXHeaderKey key) const override;
  const XEXSecurityInfo& GetSecurity() const override;
  std::span<const XEXImportLibrary> GetImports() const override;
  std::vector<u32> GetAlternateTitleIDs() const override;
  bool IsPatch() const override;
  XEXKey GetKey() const override;
  u32 GetImageSize() const override;
  [[nodiscard]] Result<void> Decode(std::span<const u8> file, std::span<u8> image) override;
  [[nodiscard]] Result<std::unique_ptr<IXEXImage>> ApplyHeaderDelta(
      const IXEXImage& patch) const override;
  [[nodiscard]] Result<void> ApplyImageDelta(const IXEXImage& patch,
                                             std::span<u8> image) const override;

 protected:
  const void* FindOptHeader(kernel::XEXHeaderKey key) const override;

 private:
  [[nodiscard]] Result<void> ReadHeader();
  [[nodiscard]] Result<void> ReadSecurityInfo();
  [[nodiscard]] Result<void> ReadImports();
  [[nodiscard]] Result<void> DecodeWith(const kernel::XEXOptFileFormatInfo& format,
                                        std::span<const u8> data, std::span<u8> image) const;
  XEXImage Clone() const;
  [[nodiscard]] Result<XEXImage> ApplyHeaderDeltaTo(const XEXImage& patch) const;

  std::vector<u8> header_;
  std::vector<u8> patch_data_;
  XEXSecurityInfo security_;
  std::vector<XEXImportLibrary> imports_;
  std::array<u8, 16> session_key_{};
  std::array<u8, 16> previous_session_key_{};
  bool xex1_ = false;
  XEXKey key_ = XEXKey::kRetail;
};

}  // namespace rex::filesystem
