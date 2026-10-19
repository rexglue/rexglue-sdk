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

#include <rex/runtime/xdbf_reader.h>

namespace rex {
namespace runtime {

constexpr memory::FourCCT kXDBFSignatureXDBF = memory::make_fourcc("XDBF");
constexpr memory::FourCCT kXDBFSignatureXSTC = memory::make_fourcc("XSTC");
constexpr memory::FourCCT kXDBFSignatureXSTR = memory::make_fourcc("XSTR");
constexpr memory::FourCCT kXDBFSignatureXACH = memory::make_fourcc("XACH");

constexpr u64 kXDBFIdTitle = 0x8000;
constexpr u64 kXDBFIdXSTC = 0x58535443;
constexpr u64 kXDBFIdXACH = 0x58414348;

namespace {

template <typename T>
const T* StructAt(const XDBFBlock& block, size_t offset) {
  if (!block || offset > block.size || sizeof(T) > block.size - offset) {
    return nullptr;
  }
  return reinterpret_cast<const T*>(block.buffer + offset);
}

}  // namespace

XDBFReader::XDBFReader(const u8* data, size_t data_size) : data_(data), data_size_(data_size) {
  if (!data || data_size <= sizeof(XDBFHeader)) {
    data_ = nullptr;
    return;
  }

  const auto* header = reinterpret_cast<const XDBFHeader*>(data);
  const u64 tables_size = u64{sizeof(XDBFEntry)} * header->entry_capacity +
                          u64{sizeof(XDBFFileLocation)} * header->free_count;
  if (header->magic != kXDBFSignatureXDBF || header->entry_used > header->entry_capacity ||
      tables_size > data_size - sizeof(XDBFHeader)) {
    data_ = nullptr;
    return;
  }

  const u8* ptr = data + sizeof(XDBFHeader);
  header_ = header;
  entries_ = reinterpret_cast<const XDBFEntry*>(ptr);
  ptr += sizeof(XDBFEntry) * header->entry_capacity;
  files_ = reinterpret_cast<const XDBFFileLocation*>(ptr);
  ptr += sizeof(XDBFFileLocation) * header->free_count;
  content_offset_ = ptr;
}

XDBFBlock XDBFReader::GetEntry(XDBFSection section, u64 id) const {
  if (!IsValid()) {
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
  const auto* xstr_head = StructAt<XDBFSectionHeader>(language_block, 0);
  if (!xstr_head || xstr_head->magic != kXDBFSignatureXSTR || xstr_head->version != 1) {
    return "";
  }

  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < xstr_head->count; ++i) {
    const auto* entry = StructAt<XDBFStringTableEntry>(language_block, offset);
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

  auto achievement_table = GetEntry(XDBFSection::kMetadata, kXDBFIdXACH);
  const auto* xach_head = StructAt<XDBFSectionHeader>(achievement_table, 0);
  if (!xach_head || xach_head->magic != kXDBFSignatureXACH || xach_head->version != 1) {
    return achievements;
  }

  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < xach_head->count; ++i) {
    const auto* entry = StructAt<XDBFAchievementTableEntry>(achievement_table, offset);
    if (!entry) {
      break;
    }
    offset += sizeof(XDBFAchievementTableEntry);
    achievements.push_back(*entry);
  }
  return achievements;
}

XLanguage XDBFTitleData::GetSupportedLanguage(XLanguage language_to_check) const {
  return GetTitle(language_to_check).empty() ? GetDefaultLanguage() : language_to_check;
}

XDBFBlock XDBFTitleData::GetIcon() const {
  return GetEntry(XDBFSection::kImage, kXDBFIdTitle);
}

XLanguage XDBFTitleData::GetDefaultLanguage() const {
  const auto* xstc = StructAt<XDBFXstc>(GetEntry(XDBFSection::kMetadata, kXDBFIdXSTC), 0);
  if (!xstc || xstc->magic != kXDBFSignatureXSTC) {
    return XLanguage::kEnglish;
  }
  return ToXLanguage(xstc->default_language);
}

std::string XDBFTitleData::GetTitle() const {
  return GetStringTableEntry(GetDefaultLanguage(), kXDBFIdTitle);
}

std::string XDBFTitleData::GetTitle(XLanguage language) const {
  return GetStringTableEntry(language, kXDBFIdTitle);
}

}  // namespace runtime
}  // namespace rex
