/**
 * @file        filesystem/xdbf_title_data.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>
#include <vector>

#include <rex/filesystem/xdbf.h>
#include <rex/kernel/types/content.h>
#include <rex/types.h>

namespace rex::filesystem {

/** An in memory XDBF database. */
class XDBFTitleData final : public IXDBFTitleData {
 public:
  /**
   * Constructs the reader and validates the database header.
   *
   * @param data The database bytes, which must outlive the reader.
   */
  XDBFTitleData(const u8* data, size_t data_size);

  bool IsValid() const override { return data_ != nullptr; }
  XDBFBlock GetEntry(XDBFSection section, u64 id) const override;
  std::string GetStringTableEntry(kernel::XLanguage language, u16 string_id) const override;
  std::vector<XDBFAchievementTableEntry> GetAchievements() const override;
  kernel::XLanguage GetSupportedLanguage(kernel::XLanguage language_to_check) const override;
  XDBFBlock GetIcon() const override;
  kernel::XLanguage GetDefaultLanguage() const override;
  std::string GetTitle() const override;
  std::string GetTitle(kernel::XLanguage language) const override;

 private:
  const u8* data_ = nullptr;
  size_t data_size_ = 0;
  const u8* content_offset_ = nullptr;

  u32 entry_used_ = 0;
  const u8* entries_ = nullptr;
};

}  // namespace rex::filesystem
