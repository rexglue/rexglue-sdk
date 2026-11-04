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

#include "xam/xam_state.h"
#include "xam/xam_ui.h"
#include <rex/logging/log_category.h>
#include <rex/math.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/string/text.h>
#include <rex/ppc/hook.h>
#include <rex/kernel/thread_registry.h>
#include <rex/types.h>
#include <rex/kernel/title_runtime.h>
#include <rex/xam/device.h>
#include "xam/enumerator_object.h"
#include <rex/kernel/types/status.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

constexpr u64 kOneGB = 1024ull * 1024ull * 1024ull;

static const rex::xam::PlaceholderDevice kDummyHDDDeviceInfo = {
    rex::xam::DummyDeviceID::kHDD,
    rex::xam::DeviceType::kHDD,
    20ull * kOneGB,
    3ull * kOneGB,
    u"Dummy HDD",
};
static const rex::xam::PlaceholderDevice kDummyODDDeviceInfo = {
    rex::xam::DummyDeviceID::kODD,
    rex::xam::DeviceType::kODD,
    7ull * kOneGB,
    0ull * kOneGB,
    u"Dummy ODD",
};
static const rex::xam::PlaceholderDevice* g_dummy_device_infos[] = {
    &kDummyHDDDeviceInfo,
    &kDummyODDDeviceInfo,
};

const rex::xam::PlaceholderDevice* GetDummyDeviceInfo(u32 device_id) {
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
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto name = std::u16string(device_info->name);
  if (name_capacity < name.size() + 1) {
    return kernel::X_ERROR_INSUFFICIENT_BUFFER;
  }
  rex::string::CopyAndSwapTruncating(name_buffer, name, name_capacity);
  return kernel::X_ERROR_SUCCESS;
}

u32 XamContentGetDeviceState_entry(u32 device_id, MappedVoid overlapped_ptr) {
  auto device_info = GetDummyDeviceInfo(device_id);
  if (device_info == nullptr) {
    if (overlapped_ptr) {
      CurrentTitle().overlapped_dispatcher.Complete(
          overlapped_ptr.GetGuestAddress(), {.result = kernel::X_ERROR_FUNCTION_FAILED,
                                             .extended_error = kernel::X_ERROR_DEVICE_NOT_CONNECTED,
                                             .length = 0});
      return kernel::X_ERROR_IO_PENDING;
    } else {
      return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
    }
  }
  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = kernel::X_ERROR_SUCCESS});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return kernel::X_ERROR_SUCCESS;
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
    return kernel::X_ERROR_DEVICE_NOT_CONNECTED;
  }
  device_data.Zero();
  device_data->device_id = static_cast<u32>(device_info->device_id);
  device_data->device_type = static_cast<u32>(device_info->device_type);
  device_data->total_bytes = device_info->total_bytes;
  device_data->free_bytes = device_info->free_bytes;
  rex::string::CopyAndSwapTruncating(device_data->name_chars, device_info->name,
                                     rex::CountOf(device_data->name_chars));
  return kernel::X_ERROR_SUCCESS;
}

u32 XamContentCreateDeviceEnumerator_entry([[maybe_unused]] u32 content_type,
                                           [[maybe_unused]] u32 content_flags, u32 max_count,
                                           MappedU32 buffer_size_ptr, MappedU32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(X_CONTENT_DEVICE_DATA) * max_count;
  }

  StaticItemList items(max_count, sizeof(X_CONTENT_DEVICE_DATA));

  for (const auto& device_info : g_dummy_device_infos) {
    auto device_data = items.Append<X_CONTENT_DEVICE_DATA>();
    REX_ASSERT(device_data != nullptr);
    if (device_data) {
      device_data->device_id = static_cast<u32>(device_info->device_id);
      device_data->device_type = static_cast<u32>(device_info->device_type);
      device_data->total_bytes = device_info->total_bytes;
      device_data->free_bytes = device_info->free_bytes;
      rex::string::CopyAndSwapTruncating(device_data->name_chars, device_info->name,
                                         rex::CountOf(device_data->name_chars));
    }
  }

  kernel::X_HANDLE handle = kernel::kXInvalidHandleValue;
  auto result = CreateEnumerator(
      &handle, EnumeratorInit{0xFE, 0xFE, 0x2000A, 0x20009, 0},
      EnumeratorObject(
          max_count, sizeof(X_CONTENT_DEVICE_DATA),
          [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
            return items.Write(buffer_data, written_count);
          }));
  if (result == kernel::X_STATUS_NO_MEMORY) {
    return kernel::X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (kernel::XFAILED(result)) {
    return result;
  }

  *handle_out = handle;
  return kernel::X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace rex

namespace rex::xam {

const PlaceholderDevice* GetPlaceholderDevice(u32 device_id) {
  return rex::xam::GetDummyDeviceInfo(device_id);
}

}  // namespace rex::xam

REX_XAM_BODY(__imp__XamContentGetDeviceName, rex::xam::XamContentGetDeviceName_entry)
REX_XAM_BODY(__imp__XamContentGetDeviceState, rex::xam::XamContentGetDeviceState_entry)
REX_XAM_BODY(__imp__XamContentGetDeviceData, rex::xam::XamContentGetDeviceData_entry)
REX_XAM_BODY(__imp__XamContentCreateDeviceEnumerator,
             rex::xam::XamContentCreateDeviceEnumerator_entry)
