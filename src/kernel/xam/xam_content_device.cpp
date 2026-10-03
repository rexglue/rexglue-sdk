/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/xenumerator.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;

// TODO(gibbed): real information.
//
// Until we expose real information about a HDD device, we
// claim there is 3GB free on a 4GB dummy HDD.
//
// There is a possibility that certain games are bugged in that
// they incorrectly only look at the lower 32-bits of free_bytes,
// when it is a 64-bit value. Which means any size above ~4GB
// will not be recognized properly.
#define ONE_GB (1024ull * 1024ull * 1024ull)

static const DummyDeviceInfo dummy_hdd_device_info_ = {
    DummyDeviceId::HDD, DeviceType::HDD,
    20ull * ONE_GB,  // 20GB
    3ull * ONE_GB,   // 3GB, so it looks a little used.
    u"Dummy HDD",
};
static const DummyDeviceInfo dummy_odd_device_info_ = {
    DummyDeviceId::ODD, DeviceType::ODD,
    7ull * ONE_GB,  // 7GB (rough maximum)
    0ull * ONE_GB,  // read-only FS, so no free space
    u"Dummy ODD",
};
static const DummyDeviceInfo* dummy_device_infos_[] = {
    &dummy_hdd_device_info_,
    &dummy_odd_device_info_,
};
#undef ONE_GB

const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id) {
  const auto& begin = std::begin(dummy_device_infos_);
  const auto& end = std::end(dummy_device_infos_);
  auto it = std::find_if(begin, end, [device_id](const auto& item) {
    return static_cast<uint32_t>(item->device_id) == device_id;
  });
  return it == end ? nullptr : *it;
}

u32 XamContentGetDeviceName_entry(u32 device_id, mapped_wstring name_buffer, u32 name_capacity) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto name = std::u16string(device_info->name);
  if (name_capacity < name.size() + 1) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  rex::copy_and_swap_truncating(name_buffer, name, name_capacity);
  return X_ERROR_SUCCESS;
}

u32 XamContentGetDeviceState_entry(u32 device_id, mapped_void overlapped_ptr) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    if (overlapped_ptr) {
      runtime::Current().dispatcher().CompleteOverlappedImmediateEx(
          overlapped_ptr.guest_address(), X_ERROR_FUNCTION_FAILED, X_ERROR_DEVICE_NOT_CONNECTED, 0);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
  }
  if (overlapped_ptr) {
    runtime::Current().dispatcher().CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}

typedef struct {
  rex::be<uint32_t> device_id;
  rex::be<uint32_t> device_type;
  rex::be<uint64_t> total_bytes;
  rex::be<uint64_t> free_bytes;
  union {
    rex::be<uint16_t> name[28];
    char16_t name_chars[28];
  };
} X_CONTENT_DEVICE_DATA;
static_assert_size(X_CONTENT_DEVICE_DATA, 0x50);

u32 XamContentGetDeviceData_entry(u32 device_id, ppc_ptr_t<X_CONTENT_DEVICE_DATA> device_data) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  device_data.Zero();
  device_data->device_id = static_cast<uint32_t>(device_info->device_id);
  device_data->device_type = static_cast<uint32_t>(device_info->device_type);
  device_data->total_bytes = device_info->total_bytes;
  device_data->free_bytes = device_info->free_bytes;
  rex::copy_and_swap_truncating(device_data->name_chars, device_info->name,
                                rex::countof(device_data->name_chars));
  return X_ERROR_SUCCESS;
}

u32 XamContentCreateDeviceEnumerator_entry(u32 content_type, u32 content_flags, u32 max_count,
                                           mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  assert_not_null(handle_out);

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(X_CONTENT_DEVICE_DATA) * max_count;
  }

  StaticItems items(max_count, sizeof(X_CONTENT_DEVICE_DATA));

  for (const auto& device_info : dummy_device_infos_) {
    // Copy our dummy device into the enumerator
    auto device_data = items.Append<X_CONTENT_DEVICE_DATA>();
    assert_not_null(device_data);
    if (device_data) {
      device_data->device_id = static_cast<uint32_t>(device_info->device_id);
      device_data->device_type = static_cast<uint32_t>(device_info->device_type);
      device_data->total_bytes = device_info->total_bytes;
      device_data->free_bytes = device_info->free_bytes;
      rex::copy_and_swap_truncating(device_data->name_chars, device_info->name,
                                    rex::countof(device_data->name_chars));
    }
  }

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  auto result = CreateEnumerator(
      runtime::Current(), &handle, EnumeratorInit{0xFE, 0xFE, 0x2000A, 0x20009, 0},
      EnumeratorObject(
          max_count, sizeof(X_CONTENT_DEVICE_DATA),
          [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
            return items.Write(buffer_data, written_count);
          }));
  if (result == X_STATUS_NO_MEMORY) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (XFAILED(result)) {
    return result;
  }

  *handle_out = handle;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

// fix me later
namespace rex {
namespace runtime {
namespace xam {

const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id) {
  return rex::kernel::xam::GetDummyDeviceInfo(device_id);
}

}  // namespace xam
}  // namespace runtime
}  // namespace rex

REX_EXPORT(__imp__XamContentGetDeviceName, rex::kernel::xam::XamContentGetDeviceName_entry)
REX_EXPORT(__imp__XamContentGetDeviceState, rex::kernel::xam::XamContentGetDeviceState_entry)
REX_EXPORT(__imp__XamContentGetDeviceData, rex::kernel::xam::XamContentGetDeviceData_entry)
REX_EXPORT(__imp__XamContentCreateDeviceEnumerator,
           rex::kernel::xam::XamContentCreateDeviceEnumerator_entry)

REX_EXPORT_STUB(__imp__XamContentAddCacheDevice);
REX_EXPORT_STUB(__imp__XamContentDeviceCheckUpdates);
REX_EXPORT_STUB(__imp__XamContentGetDefaultDevice);
REX_EXPORT_STUB(__imp__XamContentGetDeviceSerialNumber);
REX_EXPORT_STUB(__imp__XamContentGetDeviceVolumePath);
REX_EXPORT_STUB(__imp__XamContentGetLocalizedDeviceData);
REX_EXPORT_STUB(__imp__XamContentRemoveCacheDevice);
