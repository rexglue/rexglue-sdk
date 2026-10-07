#pragma once
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

#include <rex/runtime/xtypes.h>

namespace rex {
namespace runtime {

enum X_FILE_FS_INFORMATION_CLASS {
  XFileFsVolumeInformation = 1,
  XFileFsSizeInformation = 3,
  XFileFsDeviceInformation,
  XFileFsAttributeInformation = 5,
};

enum X_FILE_DEVICE_TYPE : uint32_t {
  FILE_DEVICE_UNKNOWN = 0x00000022,
  FILE_DEVICE_EHSTOR = 0x00000046,
};

#pragma pack(push, 1)

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_fs_volume_information
struct X_FILE_FS_VOLUME_INFORMATION {
  be_u64 creation_time;
  be_u32 serial_number;
  be_u32 label_length;
  uint8_t supports_objects;
  char label[1];
  uint8_t pad[6];
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_VOLUME_INFORMATION, 24);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-_file_fs_size_information
struct X_FILE_FS_SIZE_INFORMATION {
  be_u64 total_allocation_units;
  be_u64 available_allocation_units;
  be_u32 sectors_per_allocation_unit;
  be_u32 bytes_per_sector;
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_SIZE_INFORMATION, 24);

// https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_fs_attribute_information
struct X_FILE_FS_ATTRIBUTE_INFORMATION {
  be_u32 attributes;
  be_i32 component_name_max_length;
  be_u32 name_length;
  char name[1];
  uint8_t pad[3];
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_ATTRIBUTE_INFORMATION, 16);

struct X_FILE_FS_DEVICE_INFORMATION {
  be<X_FILE_DEVICE_TYPE> device_type;
  be_u32 characteristics;
};
REX_STATIC_ASSERT_SIZE(X_FILE_FS_DEVICE_INFORMATION, 8);

#pragma pack(pop)

}  // namespace runtime
}  // namespace rex
