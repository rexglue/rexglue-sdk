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

#include <optional>

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
std::optional<T> LoadAt(const XDBFBlock& block, size_t offset) {
  if (!block || offset > block.size || sizeof(T) > block.size - offset) {
    return std::nullopt;
  }
  return memory::load<T>(block.buffer + offset);
}

}  // namespace

XDBFReader::XDBFReader(const u8* data, size_t data_size) : data_(data), data_size_(data_size) {
  if (!data || data_size <= sizeof(XDBFHeader)) {
    data_ = nullptr;
    return;
  }

  const auto header = memory::load<XDBFHeader>(data);
  const u32 entry_capacity = memory::load<be_u32>(&header.entry_capacity);
  const u32 entry_used = memory::load<be_u32>(&header.entry_used);
  const u32 free_count = memory::load<be_u32>(&header.free_count);
  const u64 tables_size =
      u64{sizeof(XDBFEntry)} * entry_capacity + u64{sizeof(XDBFFileLocation)} * free_count;
  if (memory::load<be_u32>(&header.magic) != kXDBFSignatureXDBF || entry_used > entry_capacity ||
      tables_size > data_size - sizeof(XDBFHeader)) {
    data_ = nullptr;
    return;
  }

  entry_used_ = entry_used;
  entries_ = data + sizeof(XDBFHeader);
  content_offset_ =
      entries_ + sizeof(XDBFEntry) * entry_capacity + sizeof(XDBFFileLocation) * free_count;
}

XDBFBlock XDBFReader::GetEntry(XDBFSection section, u64 id) const {
  if (!IsValid()) {
    return {};
  }
  const size_t content_size = data_size_ - static_cast<size_t>(content_offset_ - data_);
  for (u32 i = 0; i < entry_used_; ++i) {
    const auto entry = memory::load<XDBFEntry>(entries_ + size_t{i} * sizeof(XDBFEntry));
    if (memory::load<be_u16>(&entry.section) == static_cast<u16>(section) &&
        memory::load<be_u64>(&entry.id) == id) {
      const u32 offset = memory::load<be_u32>(&entry.offset);
      const u32 size = memory::load<be_u32>(&entry.size);
      if (u64{offset} + size > content_size) {
        return {};
      }
      XDBFBlock block;
      block.buffer = content_offset_ + offset;
      block.size = size;
      return block;
    }
  }
  return {};
}

std::string XDBFReader::GetStringTableEntry(XLanguage language, u16 string_id) const {
  auto language_block = GetEntry(XDBFSection::kStringTable, static_cast<u64>(language));
  const auto xstr_head = LoadAt<XDBFSectionHeader>(language_block, 0);
  if (!xstr_head || memory::load<be_u32>(&xstr_head->magic) != kXDBFSignatureXSTR ||
      memory::load<be_u32>(&xstr_head->version) != 1) {
    return "";
  }

  const u16 count = memory::load<be_u16>(&xstr_head->count);
  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < count; ++i) {
    const auto entry = LoadAt<XDBFStringTableEntry>(language_block, offset);
    if (!entry) {
      return "";
    }
    offset += sizeof(XDBFStringTableEntry);
    const u16 string_length = memory::load<be_u16>(&entry->string_length);
    if (string_length > language_block.size - offset) {
      return "";
    }
    if (memory::load<be_u16>(&entry->id) == string_id) {
      return std::string(reinterpret_cast<const char*>(language_block.buffer + offset),
                         string_length);
    }
    offset += string_length;
  }
  return "";
}

std::vector<XDBFAchievementTableEntry> XDBFReader::GetAchievements() const {
  std::vector<XDBFAchievementTableEntry> achievements;

  auto achievement_table = GetEntry(XDBFSection::kMetadata, kXDBFIdXACH);
  const auto xach_head = LoadAt<XDBFSectionHeader>(achievement_table, 0);
  if (!xach_head || memory::load<be_u32>(&xach_head->magic) != kXDBFSignatureXACH ||
      memory::load<be_u32>(&xach_head->version) != 1) {
    return achievements;
  }

  const u16 count = memory::load<be_u16>(&xach_head->count);
  size_t offset = sizeof(XDBFSectionHeader);
  for (u16 i = 0; i < count; ++i) {
    const auto entry = LoadAt<XDBFAchievementTableEntry>(achievement_table, offset);
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
  const auto xstc = LoadAt<XDBFXstc>(GetEntry(XDBFSection::kMetadata, kXDBFIdXSTC), 0);
  if (!xstc || memory::load<be_u32>(&xstc->magic) != kXDBFSignatureXSTC) {
    return XLanguage::kEnglish;
  }
  return ToXLanguage(memory::load<be_u32>(&xstc->default_language));
}

std::string XDBFTitleData::GetTitle() const {
  return GetStringTableEntry(GetDefaultLanguage(), kXDBFIdTitle);
}

std::string XDBFTitleData::GetTitle(XLanguage language) const {
  return GetStringTableEntry(language, kXDBFIdTitle);
}

}  // namespace runtime
}  // namespace rex
