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
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/string.h>
#include <rex/hook.h>
#include <rex/runtime/dispatcher.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

constexpr u64 kOneGB = 1024ull * 1024ull * 1024ull;

static const PlaceholderDevice kDummyHDDDeviceInfo = {
    DummyDeviceId::kHDD, DeviceType::kHDD, 20ull * kOneGB, 3ull * kOneGB, u"Dummy HDD",
};
static const PlaceholderDevice kDummyODDDeviceInfo = {
    DummyDeviceId::kODD, DeviceType::kODD, 7ull * kOneGB, 0ull * kOneGB, u"Dummy ODD",
};
static const PlaceholderDevice* g_dummy_device_infos[] = {
    &kDummyHDDDeviceInfo,
    &kDummyODDDeviceInfo,
};

const PlaceholderDevice* GetDummyDeviceInfo(u32 device_id) {
  const auto& begin = std::begin(g_dummy_device_infos);
  const auto& end = std::end(g_dummy_device_infos);
  auto it = std::find_if(begin, end, [device_id](const auto& item) {
    return static_cast<u32>(item->device_id) == device_id;
  });
  return it == end ? nullptr : *it;
}

u32 XamContentGetDeviceName_entry(u32 device_id, MappedWString name_buffer, u32 name_capacity) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto name = std::u16string(device_info->name);
  if (name_capacity < name.size() + 1) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  rex::CopyAndSwapTruncating(name_buffer, name, name_capacity);
  return X_ERROR_SUCCESS;
}

u32 XamContentGetDeviceState_entry(u32 device_id, MappedVoid overlapped_ptr) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    if (overlapped_ptr) {
      runtime::GetCurrent().GetDispatcher().Complete(
          overlapped_ptr.GetGuestAddress(), {.result = X_ERROR_FUNCTION_FAILED,
                                             .extended_error = X_ERROR_DEVICE_NOT_CONNECTED,
                                             .length = 0});
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
  }
  if (overlapped_ptr) {
    runtime::GetCurrent().GetDispatcher().Complete(overlapped_ptr.GetGuestAddress(),
                                                   {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}

struct X_CONTENT_DEVICE_DATA {
  be_u32 device_id;
  be_u32 device_type;
  be_u64 total_bytes;
  be_u64 free_bytes;
  union {
    be_u16 name[28];
    char16_t name_chars[28];
  };
};
REX_STATIC_ASSERT_SIZE(X_CONTENT_DEVICE_DATA, 0x50);

u32 XamContentGetDeviceData_entry(u32 device_id, GuestPtrT<X_CONTENT_DEVICE_DATA> device_data) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  device_data.Zero();
  device_data->device_id = static_cast<u32>(device_info->device_id);
  device_data->device_type = static_cast<u32>(device_info->device_type);
  device_data->total_bytes = device_info->total_bytes;
  device_data->free_bytes = device_info->free_bytes;
  rex::CopyAndSwapTruncating(device_data->name_chars, device_info->name,
                             rex::CountOf(device_data->name_chars));
  return X_ERROR_SUCCESS;
}

u32 XamContentCreateDeviceEnumerator_entry([[maybe_unused]] u32 content_type,
                                           [[maybe_unused]] u32 content_flags, u32 max_count,
                                           MappedU32 buffer_size_ptr, MappedU32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(X_CONTENT_DEVICE_DATA) * max_count;
  }

  StaticItems items(max_count, sizeof(X_CONTENT_DEVICE_DATA));

  for (const auto& device_info : g_dummy_device_infos) {
    auto device_data = items.Append<X_CONTENT_DEVICE_DATA>();
    REX_ASSERT(device_data != nullptr);
    if (device_data) {
      device_data->device_id = static_cast<u32>(device_info->device_id);
      device_data->device_type = static_cast<u32>(device_info->device_type);
      device_data->total_bytes = device_info->total_bytes;
      device_data->free_bytes = device_info->free_bytes;
      rex::CopyAndSwapTruncating(device_data->name_chars, device_info->name,
                                 rex::CountOf(device_data->name_chars));
    }
  }

  X_HANDLE handle = kXInvalidHandleValue;
  auto result = CreateEnumerator(
      runtime::GetCurrent(), &handle, EnumeratorInit{0xFE, 0xFE, 0x2000A, 0x20009, 0},
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

namespace rex {
namespace runtime {

const PlaceholderDevice* GetPlaceholderDevice(u32 device_id) {
  return rex::kernel::xam::GetDummyDeviceInfo(device_id);
}

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
