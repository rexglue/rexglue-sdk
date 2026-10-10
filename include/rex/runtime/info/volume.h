/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

/** Volume information class selecting the layout of a query request. */
enum X_FILE_FS_INFORMATION_CLASS : u8 {
  XFileFsVolumeInformation = 1,    /**< Volume label and serial number. */
  XFileFsSizeInformation = 3,      /**< Volume size and allocation unit geometry. */
  XFileFsDeviceInformation,        /**< Device type and characteristics. */
  XFileFsAttributeInformation = 5, /**< File system name and capabilities. */
};

/** Device type reported for a volume. */
// NOLINTNEXTLINE(performance-enum-size): guest memory field width
enum X_FILE_DEVICE_TYPE : u32 {
  FILE_DEVICE_UNKNOWN = 0x00000022, /**< Device type the volume device query reports. */
  FILE_DEVICE_EHSTOR = 0x00000046,  /**< Device type 0x46. No code uses it. */
};

#pragma pack(push, 1)

/** Label and serial number of a volume. */
struct X_FILE_FS_VOLUME_INFORMATION {
  be_u64 creation_time; /**< Offset 0x00. Creation time. */
  be_u32 serial_number; /**< Offset 0x08. */
  be_u32 label_length;  /**< Offset 0x0C. Label length in bytes. */
  u8 supports_objects;  /**< Offset 0x10. Nonzero when the volume supports objects. */
  char label[1];        /**< Offset 0x11. First character of the label. */
  u8 pad[6];            /**< Offset 0x12. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_VOLUME_INFORMATION, 24);

/** Size and allocation unit geometry of a volume. */
struct X_FILE_FS_SIZE_INFORMATION {
  be_u64 total_allocation_units;      /**< Offset 0x00. */
  be_u64 available_allocation_units;  /**< Offset 0x08. */
  be_u32 sectors_per_allocation_unit; /**< Offset 0x10. */
  be_u32 bytes_per_sector;            /**< Offset 0x14. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_SIZE_INFORMATION, 24);

/** File system name and capabilities of a volume. */
struct X_FILE_FS_ATTRIBUTE_INFORMATION {
  be_u32 attributes;                /**< Offset 0x00. File system attribute flags. */
  be_i32 component_name_max_length; /**< Offset 0x04. Longest path component in characters. */
  be_u32 name_length;               /**< Offset 0x08. Name length in bytes. */
  char name[1];                     /**< Offset 0x0C. First character of the file system name. */
  u8 pad[3];                        /**< Offset 0x0D. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_ATTRIBUTE_INFORMATION, 16);

/** Device type and characteristics of a volume. */
struct X_FILE_FS_DEVICE_INFORMATION {
  be<X_FILE_DEVICE_TYPE> device_type; /**< Offset 0x00. */
  be_u32 characteristics;             /**< Offset 0x04. */
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_DEVICE_INFORMATION, 8);

#pragma pack(pop)

}  // namespace runtime
}  // namespace rex
