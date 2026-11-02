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
#include <rex/kernel/types/content.h>
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/math.h>
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/device.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/module.h>
#include <rex/types.h>
#include <rex/string/text.h>
#include <rex/filesystem/path.h>
#include <rex/string/numeric.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include "xam/enumerator_object.h"
#include <rex/kernel/types/status.h>
#include <rex/kernel/object/object_table.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

void AddODDContent(StaticItemList& items, rex::kernel::XContentType content_type) {
  constexpr std::string_view kRoot = "game:\\Content\\0000000000000000";
  auto* fs = (&CurrentTitle().file_system);
  auto titles = fs->List(kRoot, "????????");
  if (!titles) {
    return;
  }

  auto content_type_path = std::format("{:08X}", static_cast<u32>(content_type));

  while (auto title_entry = (*titles)->Next()) {
    auto title_id = rex::string::from_string<u32>(title_entry->name, true);

    auto contents = fs->List(
        rex::filesystem::utf8_join_guest_paths({kRoot, title_entry->name, content_type_path}),
        "????????????????");
    if (contents) {
      while (auto content_entry = (*contents)->Next()) {
        auto item = items.Append<kernel::XCONTENT_AGGREGATE_DATA>();
        REX_ASSERT(item != nullptr);
        if (item) {
          item->device_id = std::to_underlying(rex::xam::DummyDeviceID::kODD);
          item->content_type = content_type;
          item->SetDisplayName(string::ToUTF16(content_entry->name));
          item->SetFileName(content_entry->name);
          item->title_id = title_id;
        }
      }
    }
  }
}

u32 XamContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id, u32 content_type,
                                              [[maybe_unused]] u32 unk3, MappedU32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  auto device_info = device_id == 0 ? nullptr : rex::xam::GetPlaceholderDevice(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return kernel::X_E_INVALIDARG;
  }

  StaticItemList items(1, sizeof(kernel::XCONTENT_AGGREGATE_DATA));

  auto content_type_enum = static_cast<rex::kernel::XContentType>(content_type);

  u64 profile_xuid = CurrentXAM().GetUserProfile().GetXUID();

  if (!device_info || device_info->device_type == rex::xam::DeviceType::kHDD) {
    std::vector<u32> title_ids{kernel::kRunningTitleID};
    auto exe_module = CurrentTitle().objects.Get<kernel::ILoadedModule>(
        CurrentTitle().modules.GetExecutable().value_or(0));
    if (exe_module) {
      const auto alt_ids = exe_module->GetAlternateTitleIDs();
      std::copy(alt_ids.cbegin(), alt_ids.cend(), std::back_inserter(title_ids));
    }

    for (auto& title_id : title_ids) {
      auto contents = CurrentXAM().GetContentManager().ListContent(
          std::to_underlying(rex::xam::DummyDeviceID::kHDD), xuid, content_type_enum, title_id);
      for (const auto& content_data : contents) {
        auto item = items.Append<kernel::XCONTENT_AGGREGATE_DATA>();
        REX_ASSERT(item != nullptr);
        if (item) {
          *item = content_data;
        }
      }

      if (profile_xuid != 0) {
        auto common_datas = CurrentXAM().GetContentManager().ListContent(
            std::to_underlying(rex::xam::DummyDeviceID::kHDD), 0, content_type_enum, title_id);
        for (const auto& content_data : common_datas) {
          auto item = items.Append<kernel::XCONTENT_AGGREGATE_DATA>();
          REX_ASSERT(item != nullptr);
          if (item) {
            *item = content_data;
          }
        }
      }
    }
  }

  if (!device_info || device_info->device_type == rex::xam::DeviceType::kODD) {
    AddODDContent(items, content_type_enum);
  }

  REXLOG_DEBUG(xam, "XamContentAggregateCreateEnumerator: added {} items to enumerator",
               items.GetItemCount());

  kernel::X_HANDLE handle = kernel::kXInvalidHandleValue;
  void* extra_buffer = nullptr;
  auto result = CreateEnumerator(
      &handle, EnumeratorInit{0xFF, 0xFE, 0x2000E, 0x20010, 0},
      EnumeratorObject(
          1, sizeof(kernel::XCONTENT_AGGREGATE_DATA),
          [items = std::move(items)](u32, u8* buffer_data, u32* written_count) mutable {
            return items.Write(buffer_data, written_count);
          }),
      sizeof(X_KENUMERATOR_CONTENT_AGGREGATE), &extra_buffer);
  if (result == kernel::X_STATUS_NO_MEMORY) {
    return kernel::X_ERROR_NOT_ENOUGH_MEMORY;
  }
  if (kernel::XFAILED(result)) {
    return result;
  }

  auto* extra = static_cast<X_KENUMERATOR_CONTENT_AGGREGATE*>(extra_buffer);
  extra->magic = kernel::kStashSignature;
  extra->handle = handle;

  *handle_out = handle;
  return kernel::X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamContentAggregateCreateEnumerator,
             rex::xam::XamContentAggregateCreateEnumerator_entry)
