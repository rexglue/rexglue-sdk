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

#include <rex/filesystem.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <rex/runtime.h>
#include <rex/runtime/user_module.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/xenumerator.h>
#include <rex/runtime/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;
using namespace rex::runtime::xam;

void AddODDContentTest(StaticItems& items, XContentType content_type) {
  constexpr std::string_view kRoot = "game:\\Content\\0000000000000000";
  auto* fs = runtime::Current().file_system();
  auto titles = fs->List(kRoot, "????????");
  if (!titles) {
    return;
  }

  auto content_type_path = std::format("{:08X}", static_cast<u32>(content_type));

  while (auto title_entry = (*titles)->Next()) {
    auto title_id = rex::from_string<u32>(title_entry->name, true);

    auto contents =
        fs->List(rex::utf8_join_guest_paths({kRoot, title_entry->name, content_type_path}),
                 "????????????????");
    if (contents) {
      while (auto content_entry = (*contents)->Next()) {
        auto item = items.Append<XCONTENT_AGGREGATE_DATA>();
        REX_ASSERT(item != nullptr);
        if (item) {
          item->device_id = std::to_underlying(DummyDeviceId::ODD);
          item->content_type = content_type;
          item->set_display_name(to_utf16(content_entry->name));
          item->set_file_name(content_entry->name);
          item->title_id = title_id;
        }
      }
    }
  }
}

u32 XamContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id, u32 content_type,
                                              [[maybe_unused]] u32 unk3, mapped_u32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return X_E_INVALIDARG;
  }

  StaticItems items(1, sizeof(XCONTENT_AGGREGATE_DATA));

  auto content_type_enum = XContentType(static_cast<u32>(content_type));

  u64 userxuid = runtime::Current().content_manager().user_profile().xuid();

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    std::vector<u32> title_ids{kCurrentlyRunningTitleId};
    auto exe_module = runtime::Current().object_table().Get<ModuleObject>(
        runtime::Current().module_registry().executable().value_or(0));
    if (exe_module && UserModuleXEX(*exe_module)) {
      const auto alt_ids = UserModuleXEX(*exe_module)->image().alternate_title_ids();
      std::copy(alt_ids.cbegin(), alt_ids.cend(), std::back_inserter(title_ids));
    }

    for (auto& title_id : title_ids) {
      auto content_datas = runtime::Current().content_manager().ListContent(
          std::to_underlying(DummyDeviceId::HDD), xuid, content_type_enum, title_id);
      for (const auto& content_data : content_datas) {
        auto item = items.Append<XCONTENT_AGGREGATE_DATA>();
        REX_ASSERT(item != nullptr);
        if (item) {
          *item = content_data;
        }
      }

      if (userxuid != 0) {
        auto common_datas = runtime::Current().content_manager().ListContent(
            std::to_underlying(DummyDeviceId::HDD), 0, content_type_enum, title_id);
        for (const auto& content_data : common_datas) {
          auto item = items.Append<XCONTENT_AGGREGATE_DATA>();
          REX_ASSERT(item != nullptr);
          if (item) {
            *item = content_data;
          }
        }
      }
    }
  }

  if (!device_info || device_info->device_type == DeviceType::ODD) {
    AddODDContentTest(items, content_type_enum);
  }

  REXLOG_DEBUG(kernel, "XamContentAggregateCreateEnumerator: added {} items to enumerator",
               items.item_count());

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  void* extra_buffer = nullptr;
  auto result = CreateEnumerator(
      runtime::Current(), &handle, EnumeratorInit{0xFF, 0xFE, 0x2000E, 0x20010, 0},
      EnumeratorObject(
          1, sizeof(XCONTENT_AGGREGATE_DATA),
          [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
            return items.Write(buffer_data, written_count);
          }),
      sizeof(X_KENUMERATOR_CONTENT_AGGREGATE), &extra_buffer);
  if (result == X_STATUS_NO_MEMORY) {
    return X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (XFAILED(result)) {
    return result;
  }

  auto* extra = static_cast<X_KENUMERATOR_CONTENT_AGGREGATE*>(extra_buffer);
  extra->magic = kXObjSignature;
  extra->handle = handle;

  *handle_out = handle;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamContentAggregateCreateEnumerator,
           rex::kernel::xam::XamContentAggregateCreateEnumerator_entry)
