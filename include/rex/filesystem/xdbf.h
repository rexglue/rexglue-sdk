/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        filesystem/xdbf.h
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <string>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/filesystem/xex.h>
#include <rex/kernel/types/content.h>

namespace rex {
namespace filesystem {

enum class XDBFSection : u8 {
  kMetadata = 0x0001,
  kImage = 0x0002,
  kStringTable = 0x0003, /**< String tables, one per language. */
};

#pragma pack(push, 1)
struct XDBFHeader {
  be_u32 magic;
  be_u32 version;
  be_u32 entry_capacity;
  be_u32 entry_used;
  be_u32 free_count; /**< Rows in the table after the entry table. */
  be_u32 free_used;  /**< Rows in use in the table after the entry table. */
};
REX_STATIC_ASSERT_SIZE(XDBFHeader, 24);

/** Entry table row locating the data of an XDBF entry. */
struct XDBFEntry {
  be_u16 section;
  be_u64 id;
  be_u32 offset; /**< Offset of the data from the content area. */
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XDBFEntry, 18);

/** Row of the table between the entry table and the content area. */
struct XDBFFileLocation {
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XDBFFileLocation, 8);

/** Metadata entry holding the default language of the title. */
struct XDBFXSTC {
  be_u32 magic;
  be_u32 version;
  be_u32 size;
  be_u32 default_language; /**< XLanguage value. */
};
REX_STATIC_ASSERT_SIZE(XDBFXSTC, 16);

/** Header of an XSTR string table or an XACH achievement table. */
struct XDBFSectionHeader {
  be_u32 magic;
  be_u32 version;
  be_u32 size;
  be_u16 count;
};
REX_STATIC_ASSERT_SIZE(XDBFSectionHeader, 14);

/** Header of a string table entry, followed by the text. */
struct XDBFStringTableEntry {
  be_u16 id;
  be_u16 string_length; /**< Length of the text in bytes. */
};
REX_STATIC_ASSERT_SIZE(XDBFStringTableEntry, 4);

struct XDBFAchievementTableEntry {
  be_u16 id;
  be_u16 label_id;       /**< String ID of the name. */
  be_u16 description_id; /**< String ID of the description shown once unlocked. */
  be_u16 unachieved_id;  /**< String ID of the description shown while locked. */
  be_u32 image_id;       /**< Image entry ID of the icon. */
  be_u16 gamerscore;
  be_u16 unk0E;
  be_u32 flags;
  be_u32 unk14;
  be_u32 unk18;
  be_u32 unk1c;
  be_u32 unk20;
};
REX_STATIC_ASSERT_SIZE(XDBFAchievementTableEntry, 0x24);
#pragma pack(pop)

/** Reference to the bytes of an XDBF entry inside the database. */
struct XDBFBlock {
  const u8* buffer; /**< First byte of the entry, null when absent. */
  size_t size;

  operator bool() const { return buffer != nullptr; }
};

/** Reads an in memory XDBF database. Create one with IFileSystemFactory::OpenXDBF. */
class IXDBFReader {
 public:
  virtual ~IXDBFReader() = default;

  virtual bool IsValid() const = 0;

  /** Returns an entry of a section, with a null buffer when absent. */
  virtual XDBFBlock GetEntry(XDBFSection section, u64 id) const = 0;

  /**
   * @return The string, empty when absent.
   */
  virtual std::string GetStringTableEntry(kernel::XLanguage language, u16 string_id) const = 0;

  virtual std::vector<XDBFAchievementTableEntry> GetAchievements() const = 0;
};

class IXDBFTitleData : public IXDBFReader {
 public:
  /** Returns the language when the database has it, else the default language. */
  virtual kernel::XLanguage GetSupportedLanguage(kernel::XLanguage language_to_check) const = 0;

  virtual XDBFBlock GetIcon() const = 0;

  virtual kernel::XLanguage GetDefaultLanguage() const = 0;

  virtual std::string GetTitle() const = 0;

  /** Returns the title name in a language, empty when absent. */
  virtual std::string GetTitle(kernel::XLanguage language) const = 0;
};

}  // namespace filesystem
}  // namespace rex
