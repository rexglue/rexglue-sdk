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

#include <rex/runtime/xdbf.h>

namespace rex {
namespace runtime {

constexpr memory::fourcc_t kXDBFSignatureXDBF = memory::make_fourcc("XDBF");
constexpr memory::fourcc_t kXDBFSignatureXstc = memory::make_fourcc("XSTC");
constexpr memory::fourcc_t kXDBFSignatureXstr = memory::make_fourcc("XSTR");
constexpr memory::fourcc_t kXDBFSignatureXach = memory::make_fourcc("XACH");

constexpr u64 kXDBFIdTitle = 0x8000;
constexpr u64 kXDBFIdXstc = 0x58535443;
constexpr u64 kXDBFIdXach = 0x58414348;

namespace {

template <typename T>
const T* RecordAt(const XDBFBlock& block, size_t offset) {
  if (!block || offset > block.size || sizeof(T) > block.size - offset) {
    return nullptr;
  }
  return reinterpret_cast<const T*>(block.buffer + offset);
}

}  // namespace

XDBFReader::XDBFReader(const u8* data, size_t data_size) : data_(data), data_size_(data_size) {
  if (!data || data_size <= sizeof(XbdfHeader)) {
    data_ = nullptr;
    return;
  }

  const auto* header = reinterpret_cast<const XbdfHeader*>(data);
  const u64 tables_size =
      u64{sizeof(XbdfEntry)} * header->entry_count + u64{sizeof(XbdfFileLoc)} * header->free_count;
  if (header->magic != kXDBFSignatureXDBF || header->entry_used > header->entry_count ||
      tables_size > data_size - sizeof(XbdfHeader)) {
    data_ = nullptr;
    return;
  }

  const u8* ptr = data + sizeof(XbdfHeader);
  header_ = header;
  entries_ = reinterpret_cast<const XbdfEntry*>(ptr);
  ptr += sizeof(XbdfEntry) * header->entry_count;
  files_ = reinterpret_cast<const XbdfFileLoc*>(ptr);
  ptr += sizeof(XbdfFileLoc) * header->free_count;
  content_offset_ = ptr;
}

XDBFBlock XDBFReader::GetEntry(XDBFSection section, u64 id) const {
  if (!is_valid()) {
    return {};
  }
  const size_t content_size = data_size_ - static_cast<size_t>(content_offset_ - data_);
  for (u32 i = 0; i < header_->entry_used; ++i) {
    auto& entry = entries_[i];
    if (entry.section == static_cast<u16>(section) && entry.id == id) {
      if (u64{entry.offset} + entry.size > content_size) {
        return {};
      }
      XDBFBlock block;
      block.buffer = content_offset_ + entry.offset;
      block.size = entry.size;
      return block;
    }
  }
  return {};
}

std::string XDBFReader::GetStringTableEntry(XLanguage language, u16 string_id) const {
  auto language_block = GetEntry(XDBFSection::kStringTable, static_cast<u64>(language));
  const auto* xstr_head = RecordAt<XDBFSectionHeader>(language_block, 0);
  if (!xstr_head) {
    return "";
  }
  REX_ASSERT(xstr_head->magic == kXDBFSignatureXstr);
  REX_ASSERT(xstr_head->version == 1);

  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < xstr_head->count; ++i) {
    const auto* entry = RecordAt<XDBFStringTableEntry>(language_block, offset);
    if (!entry) {
      return "";
    }
    offset += sizeof(XDBFStringTableEntry);
    if (entry->string_length > language_block.size - offset) {
      return "";
    }
    if (entry->id == string_id) {
      return std::string(reinterpret_cast<const char*>(language_block.buffer + offset),
                         entry->string_length);
    }
    offset += entry->string_length;
  }
  return "";
}

std::vector<XDBFAchievementTableEntry> XDBFReader::GetAchievements() const {
  std::vector<XDBFAchievementTableEntry> achievements;

  auto achievement_table = GetEntry(XDBFSection::kMetadata, kXDBFIdXach);
  const auto* xach_head = RecordAt<XDBFSectionHeader>(achievement_table, 0);
  if (!xach_head) {
    return achievements;
  }
  REX_ASSERT(xach_head->magic == kXDBFSignatureXach);
  REX_ASSERT(xach_head->version == 1);

  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < xach_head->count; ++i) {
    const auto* entry = RecordAt<XDBFAchievementTableEntry>(achievement_table, offset);
    if (!entry) {
      break;
    }
    offset += sizeof(XDBFAchievementTableEntry);
    achievements.push_back(*entry);
  }
  return achievements;
}

XLanguage XDBFGameData::GetExistingLanguage(XLanguage language_to_check) const {
  return title(language_to_check).empty() ? default_language() : language_to_check;
}

XDBFBlock XDBFGameData::icon() const {
  return GetEntry(XDBFSection::kImage, kXDBFIdTitle);
}

XLanguage XDBFGameData::default_language() const {
  const auto* xstc = RecordAt<XDBFXstc>(GetEntry(XDBFSection::kMetadata, kXDBFIdXstc), 0);
  if (!xstc) {
    return XLanguage::kEnglish;
  }
  REX_ASSERT(xstc->magic == kXDBFSignatureXstc);
  return ToXLanguage(xstc->default_language);
}

std::string XDBFGameData::title() const {
  return GetStringTableEntry(default_language(), kXDBFIdTitle);
}

std::string XDBFGameData::title(XLanguage language) const {
  return GetStringTableEntry(language, kXDBFIdTitle);
}

}  // namespace runtime
}  // namespace rex
