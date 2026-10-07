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

#pragma once

#include <string>
#include <vector>

#include <rex/macros.h>
#include <rex/memory.h>
#include <rex/runtime/xcontent.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {
namespace util {

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/XEX/SPA.h
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/XEX/SPA.cpp

enum class XdbfSection : uint16_t {
  kMetadata = 0x0001,
  kImage = 0x0002,
  kStringTable = 0x0003,
};

#pragma pack(push, 1)
struct XbdfHeader {
  be_u32 magic;
  be_u32 version;
  be_u32 entry_count;
  be_u32 entry_used;
  be_u32 free_count;
  be_u32 free_used;
};
REX_STATIC_ASSERT_SIZE(XbdfHeader, 24);

struct XbdfEntry {
  be_u16 section;
  be_u64 id;
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XbdfEntry, 18);

struct XbdfFileLoc {
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XbdfFileLoc, 8);

struct XdbfXstc {
  be_u32 magic;
  be_u32 version;
  be_u32 size;
  be_u32 default_language;
};
REX_STATIC_ASSERT_SIZE(XdbfXstc, 16);

struct XdbfSectionHeader {
  be_u32 magic;
  be_u32 version;
  be_u32 size;
  be_u16 count;
};
REX_STATIC_ASSERT_SIZE(XdbfSectionHeader, 14);

struct XdbfStringTableEntry {
  be_u16 id;
  be_u16 string_length;
};
REX_STATIC_ASSERT_SIZE(XdbfStringTableEntry, 4);

struct XdbfAchievementTableEntry {
  be_u16 id;
  be_u16 label_id;
  be_u16 description_id;
  be_u16 unachieved_id;
  be_u32 image_id;
  be_u16 gamerscore;
  be_u16 unkE;
  be_u32 flags;
  be_u32 unk14;
  be_u32 unk18;
  be_u32 unk1C;
  be_u32 unk20;
};
REX_STATIC_ASSERT_SIZE(XdbfAchievementTableEntry, 0x24);
#pragma pack(pop)

struct XdbfBlock {
  const uint8_t* buffer;
  size_t size;

  operator bool() const { return buffer != nullptr; }
};

// Wraps an XBDF (XboxDataBaseFormat) in-memory database.
// https://free60project.github.io/wiki/XDBF.html
class REX_RUNTIME_API XdbfWrapper {
 public:
  XdbfWrapper(const uint8_t* data, size_t data_size);

  // True if the target memory contains a valid XDBF instance.
  bool is_valid() const { return data_ != nullptr; }

  // Gets an entry in the given section.
  // If the entry is not found the returned block will be nullptr.
  XdbfBlock GetEntry(XdbfSection section, uint64_t id) const;

  // Gets a string from the string table in the given language.
  // Returns the empty string if the entry is not found.
  std::string GetStringTableEntry(XLanguage language, uint16_t string_id) const;
  std::vector<XdbfAchievementTableEntry> GetAchievements() const;

 private:
  const uint8_t* data_ = nullptr;
  size_t data_size_ = 0;
  const uint8_t* content_offset_ = nullptr;

  const XbdfHeader* header_ = nullptr;
  const XbdfEntry* entries_ = nullptr;
  const XbdfFileLoc* files_ = nullptr;
};

class REX_RUNTIME_API XDBFGameData : public XdbfWrapper {
 public:
  XDBFGameData(const uint8_t* data, size_t data_size) : XdbfWrapper(data, data_size) {}

  // Checks if provided language exist, if not returns default title language.
  XLanguage GetExistingLanguage(XLanguage language_to_check) const;

  // The game icon image, if found.
  XdbfBlock icon() const;

  // The game's default language.
  XLanguage default_language() const;

  // The game's title in its default language.
  std::string title() const;

  std::string title(XLanguage language) const;
};

}  // namespace util
}  // namespace runtime
}  // namespace rex
