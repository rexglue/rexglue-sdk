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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/memory.h>
#include <rex/runtime/xcontent_data.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/** Section of an XDBF entry. */
enum class XDBFSection : u8 {
  kMetadata = 0x0001,    /**< Metadata entries. */
  kImage = 0x0002,       /**< Image entries. */
  kStringTable = 0x0003, /**< String tables, 1 per language. */
};

#pragma pack(push, 1)
/** Header of an XDBF database. */
struct XDBFHeader {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 entry_capacity; /**< Capacity of the entry table. */
  be_u32 entry_used;     /**< Entries in use. */
  be_u32 free_count;     /**< Rows in the table after the entry table. */
  be_u32 free_used;
};
REX_STATIC_ASSERT_SIZE(XDBFHeader, 24);

/** Entry table row locating the data of an XDBF entry. */
struct XDBFEntry {
  be_u16 section; /**< XDBFSection value. */
  be_u64 id;      /**< Entry id within the section. */
  be_u32 offset;  /**< Offset of the data from the content area. */
  be_u32 size;    /**< Size in bytes. */
};
REX_STATIC_ASSERT_SIZE(XDBFEntry, 18);

/** Row of the table between the entry table and the content area. */
struct XDBFFileLocation {
  be_u32 offset;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XDBFFileLocation, 8);

/** Metadata entry holding the default language of the title. */
struct XDBFXstc {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 size;
  be_u32 default_language; /**< XLanguage value. */
};
REX_STATIC_ASSERT_SIZE(XDBFXstc, 16);

/** Header of an XSTR string table or an XACH achievement table. */
struct XDBFSectionHeader {
  be_u32 magic; /**< Signature. */
  be_u32 version;
  be_u32 size;
  be_u16 count; /**< Number of table entries. */
};
REX_STATIC_ASSERT_SIZE(XDBFSectionHeader, 14);

/** Header of a string table entry, followed by the text. */
struct XDBFStringTableEntry {
  be_u16 id;            /**< String id. */
  be_u16 string_length; /**< Length of the text in bytes. */
};
REX_STATIC_ASSERT_SIZE(XDBFStringTableEntry, 4);

/** Achievement table entry. */
struct XDBFAchievementTableEntry {
  be_u16 id;             /**< Achievement id. */
  be_u16 label_id;       /**< String id of the name. */
  be_u16 description_id; /**< String id of the description shown once unlocked. */
  be_u16 unachieved_id;  /**< String id of the description shown while locked. */
  be_u32 image_id;       /**< Image entry id of the icon. */
  be_u16 gamerscore;     /**< Gamerscore awarded. */
  be_u16 unke;
  be_u32 flags; /**< Achievement flag bits. */
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
  size_t size;      /**< Size in bytes. */

  /**
   * Reports whether the block refers to data.
   *
   * @return True when the buffer is not null.
   */
  operator bool() const { return buffer != nullptr; }
};

}  // namespace runtime
}  // namespace rex
