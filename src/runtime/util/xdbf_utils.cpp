/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/runtime/util/xdbf_utils.h>

namespace rex {
namespace runtime {
namespace util {

constexpr memory::fourcc_t kXdbfSignatureXdbf = memory::make_fourcc("XDBF");
constexpr memory::fourcc_t kXdbfSignatureXstc = memory::make_fourcc("XSTC");
constexpr memory::fourcc_t kXdbfSignatureXstr = memory::make_fourcc("XSTR");
constexpr memory::fourcc_t kXdbfSignatureXach = memory::make_fourcc("XACH");

constexpr u64 kXdbfIdTitle = 0x8000;
constexpr u64 kXdbfIdXstc = 0x58535443;
constexpr u64 kXdbfIdXach = 0x58414348;

XdbfWrapper::XdbfWrapper(const u8* data, size_t data_size) : data_(data), data_size_(data_size) {
  if (!data || data_size <= sizeof(XbdfHeader)) {
    data_ = nullptr;
    return;
  }

  const u8* ptr = data_;

  header_ = reinterpret_cast<const XbdfHeader*>(ptr);
  ptr += sizeof(XbdfHeader);
  if (header_->magic != kXdbfSignatureXdbf) {
    data_ = nullptr;
    return;
  }

  entries_ = reinterpret_cast<const XbdfEntry*>(ptr);
  ptr += sizeof(XbdfEntry) * header_->entry_count;

  files_ = reinterpret_cast<const XbdfFileLoc*>(ptr);
  ptr += sizeof(XbdfFileLoc) * header_->free_count;

  content_offset_ = ptr;
}

XdbfBlock XdbfWrapper::GetEntry(XdbfSection section, u64 id) const {
  for (u32 i = 0; i < header_->entry_used; ++i) {
    auto& entry = entries_[i];
    if (entry.section == static_cast<u16>(section) && entry.id == id) {
      XdbfBlock block;
      block.buffer = content_offset_ + entry.offset;
      block.size = entry.size;
      return block;
    }
  }
  return {};
}

std::string XdbfWrapper::GetStringTableEntry(XLanguage language, u16 string_id) const {
  auto language_block = GetEntry(XdbfSection::kStringTable, static_cast<u64>(language));
  if (!language_block) {
    return "";
  }

  auto xstr_head = reinterpret_cast<const XdbfSectionHeader*>(language_block.buffer);
  REX_ASSERT(xstr_head->magic == kXdbfSignatureXstr);
  REX_ASSERT(xstr_head->version == 1);

  const u8* ptr = language_block.buffer + sizeof(XdbfSectionHeader);
  for (u16 i = 0; i < xstr_head->count; ++i) {
    auto entry = reinterpret_cast<const XdbfStringTableEntry*>(ptr);
    ptr += sizeof(XdbfStringTableEntry);
    if (entry->id == string_id) {
      return std::string(reinterpret_cast<const char*>(ptr), entry->string_length);
    }
    ptr += entry->string_length;
  }
  return "";
}

std::vector<XdbfAchievementTableEntry> XdbfWrapper::GetAchievements() const {
  std::vector<XdbfAchievementTableEntry> achievements;

  auto achievement_table = GetEntry(XdbfSection::kMetadata, kXdbfIdXach);
  if (!achievement_table) {
    return achievements;
  }

  auto xach_head = reinterpret_cast<const XdbfSectionHeader*>(achievement_table.buffer);
  REX_ASSERT(xach_head->magic == kXdbfSignatureXach);
  REX_ASSERT(xach_head->version == 1);

  const u8* ptr = achievement_table.buffer + sizeof(XdbfSectionHeader);
  for (u16 i = 0; i < xach_head->count; ++i) {
    auto entry = reinterpret_cast<const XdbfAchievementTableEntry*>(ptr);
    ptr += sizeof(XdbfAchievementTableEntry);
    achievements.push_back(*entry);
  }
  return achievements;
}

XLanguage XDBFGameData::GetExistingLanguage(XLanguage language_to_check) const {
  return title(language_to_check).empty() ? default_language() : language_to_check;
}

XdbfBlock XDBFGameData::icon() const {
  return GetEntry(XdbfSection::kImage, kXdbfIdTitle);
}

XLanguage XDBFGameData::default_language() const {
  auto block = GetEntry(XdbfSection::kMetadata, kXdbfIdXstc);
  if (!block.buffer) {
    return XLanguage::kEnglish;
  }
  auto xstc = reinterpret_cast<const XdbfXstc*>(block.buffer);
  REX_ASSERT(xstc->magic == kXdbfSignatureXstc);
  return static_cast<XLanguage>(static_cast<u32>(xstc->default_language));
}

std::string XDBFGameData::title() const {
  return GetStringTableEntry(default_language(), kXdbfIdTitle);
}

std::string XDBFGameData::title(XLanguage language) const {
  return GetStringTableEntry(language, kXdbfIdTitle);
}

}  // namespace util
}  // namespace runtime
}  // namespace rex
