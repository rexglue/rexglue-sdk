/**
 * @file        runtime/xdbf_reader.h
 * @brief       Readers over an in memory XDBF database and the XDBF data of a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/kernel/util/xdbf_utils.h (Ben Vanik, 2016).
 */

#pragma once

#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/xcontent_data.h>
#include <rex/runtime/xdbf.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/** Wraps an in memory XDBF database. */
class REX_RUNTIME_API XDBFReader {
 public:
  /**
   * Constructs the wrapper and validates the database header.
   *
   * @param data The database bytes, which must outlive the wrapper.
   * @param data_size Size in bytes.
   */
  XDBFReader(const u8* data, size_t data_size);

  /**
   * Reports whether the target memory contains a valid XDBF instance.
   *
   * @return True when valid.
   */
  bool IsValid() const { return data_ != nullptr; }

  /**
   * Returns an entry in the given section.
   *
   * @param section The section.
   * @param id The entry id.
   * @return The block, with a null buffer when the entry is not found.
   */
  XDBFBlock GetEntry(XDBFSection section, u64 id) const;

  /**
   * Returns a string from the string table in the given language.
   *
   * @param language The language.
   * @param string_id The string id.
   * @return The string, empty when the entry is not found.
   */
  std::string GetStringTableEntry(XLanguage language, u16 string_id) const;
  /**
   * Returns every entry of the achievement table.
   *
   * @return The entries, empty when the table is absent.
   */
  std::vector<XDBFAchievementTableEntry> GetAchievements() const;

 private:
  const u8* data_ = nullptr;
  size_t data_size_ = 0;
  const u8* content_offset_ = nullptr;

  u32 entry_used_ = 0;
  const u8* entries_ = nullptr;
};

/** Typed access to the title metadata of an XDBF database. */
class REX_RUNTIME_API XDBFTitleData : public XDBFReader {
 public:
  /**
   * Constructs the accessor over a database.
   *
   * @param data The database bytes, which must outlive the accessor.
   * @param data_size Size in bytes.
   */
  XDBFTitleData(const u8* data, size_t data_size) : XDBFReader(data, data_size) {}

  /**
   * Returns the given language when the database has it, else the default title language.
   *
   * @param language_to_check The language to look up.
   * @return The language to use.
   */
  XLanguage GetSupportedLanguage(XLanguage language_to_check) const;

  /**
   * Returns the title icon image.
   *
   * @return The block, with a null buffer when absent.
   */
  XDBFBlock GetIcon() const;

  /**
   * Returns the default language of the title.
   *
   * @return The language.
   */
  XLanguage GetDefaultLanguage() const;

  /**
   * Returns the title name in its default language.
   *
   * @return The name.
   */
  std::string GetTitle() const;

  /**
   * Returns the title name in the given language.
   *
   * @param language The language.
   * @return The name, empty when the database has no string for it.
   */
  std::string GetTitle(XLanguage language) const;
};

}  // namespace runtime
}  // namespace rex
