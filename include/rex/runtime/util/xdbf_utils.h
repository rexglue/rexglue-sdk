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

/** Section of an XDBF entry. */
enum class XdbfSection : u8 {
  kMetadata = 0x0001,    /**< Metadata entries. */
  kImage = 0x0002,       /**< Image entries. */
  kStringTable = 0x0003, /**< String tables, 1 per language. */
};

#pragma pack(push, 1)
/** Header of an XDBF database. */
struct XbdfHeader {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 entry_count; /**< Capacity of the entry table. */
  be_u32 entry_used;  /**< Entries in use. */
  be_u32 free_count;  /**< Rows in the table after the entry table. */
  be_u32 free_used;
};
REX_STATIC_ASSERT_SIZE(XbdfHeader, 24);

/** Entry table row locating the data of an XDBF entry. */
struct XbdfEntry {
  be_u16 section; /**< XdbfSection value. */
  be_u64 id;      /**< Entry id within the section. */
  be_u32 offset;  /**< Offset of the data from the content area. */
  be_u32 size;    /**< Size in bytes. */
};
REX_STATIC_ASSERT_SIZE(XbdfEntry, 18);

/** Row of the table between the entry table and the content area. */
struct XbdfFileLoc {
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XbdfFileLoc, 8);

/** Metadata entry holding the default language of the title. */
struct XdbfXstc {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 size;
  be_u32 default_language; /**< XLanguage value. */
};
REX_STATIC_ASSERT_SIZE(XdbfXstc, 16);

/** Header of an XSTR string table or an XACH achievement table. */
struct XdbfSectionHeader {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 size;
  be_u16 count; /**< Number of table entries. */
};
REX_STATIC_ASSERT_SIZE(XdbfSectionHeader, 14);

/** Header of a string table entry, followed by the text. */
struct XdbfStringTableEntry {
  be_u16 id;            /**< String id. */
  be_u16 string_length; /**< Length of the text in bytes. */
};
REX_STATIC_ASSERT_SIZE(XdbfStringTableEntry, 4);

/** Achievement table entry. */
struct XdbfAchievementTableEntry {
  be_u16 id;             /**< Achievement id. */
  be_u16 label_id;       /**< String id of the name. */
  be_u16 description_id; /**< String id of the description shown once unlocked. */
  be_u16 unachieved_id;  /**< String id of the description shown while locked. */
  be_u32 image_id;       /**< Image entry id of the icon. */
  be_u16 gamerscore;     /**< Gamerscore awarded. */
  be_u16 unkE;
  be_u32 flags; /**< Achievement flag bits. */
  be_u32 unk14;
  be_u32 unk18;
  be_u32 unk1C;
  be_u32 unk20;
};
REX_STATIC_ASSERT_SIZE(XdbfAchievementTableEntry, 0x24);
#pragma pack(pop)

/** Reference to the bytes of an XDBF entry inside the database. */
struct XdbfBlock {
  const u8* buffer; /**< First byte of the entry, null when absent. */
  size_t size;      /**< Size in bytes. */

  /**
   * Reports whether the block refers to data.
   *
   * @return True when the buffer is not null.
   */
  operator bool() const { return buffer != nullptr; }
};

/** Wraps an in memory XDBF database. */
class REX_RUNTIME_API XdbfWrapper {
 public:
  /**
   * Constructs the wrapper and validates the database header.
   *
   * @param data The database bytes, which must outlive the wrapper.
   * @param data_size Size in bytes.
   */
  XdbfWrapper(const u8* data, size_t data_size);

  /**
   * Reports whether the target memory contains a valid XDBF instance.
   *
   * @return True when valid.
   */
  bool is_valid() const { return data_ != nullptr; }

  /**
   * Returns an entry in the given section.
   *
   * @param section The section.
   * @param id The entry id.
   * @return The block, with a null buffer when the entry is not found.
   */
  XdbfBlock GetEntry(XdbfSection section, u64 id) const;

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
  std::vector<XdbfAchievementTableEntry> GetAchievements() const;

 private:
  const u8* data_ = nullptr;
  size_t data_size_ = 0;
  const u8* content_offset_ = nullptr;

  const XbdfHeader* header_ = nullptr;
  const XbdfEntry* entries_ = nullptr;
  const XbdfFileLoc* files_ = nullptr;
};

/** Typed access to the title metadata of an XDBF database. */
class REX_RUNTIME_API XDBFGameData : public XdbfWrapper {
 public:
  /**
   * Constructs the accessor over a database.
   *
   * @param data The database bytes, which must outlive the accessor.
   * @param data_size Size in bytes.
   */
  XDBFGameData(const u8* data, size_t data_size) : XdbfWrapper(data, data_size) {}

  /**
   * Returns the given language when the database has it, else the default title language.
   *
   * @param language_to_check The language to look up.
   * @return The language to use.
   */
  XLanguage GetExistingLanguage(XLanguage language_to_check) const;

  /**
   * Returns the title icon image.
   *
   * @return The block, with a null buffer when absent.
   */
  XdbfBlock icon() const;

  /**
   * Returns the default language of the title.
   *
   * @return The language.
   */
  XLanguage default_language() const;

  /**
   * Returns the title name in its default language.
   *
   * @return The name.
   */
  std::string title() const;

  /**
   * Returns the title name in the given language.
   *
   * @param language The language.
   * @return The name, empty when the database has no string for it.
   */
  std::string title(XLanguage language) const;
};

}  // namespace util
}  // namespace runtime
}  // namespace rex
