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

#include <rex/filesystem/file.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string/util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

void AddODDContentTest(object_ref<XStaticEnumerator<XCONTENT_AGGREGATE_DATA>> e,
                       XContentType content_type) {
  auto root_entry = REX_KERNEL_FS()->ResolvePath("game:\\Content\\0000000000000000");
  if (!root_entry) {
    return;
  }

  auto content_type_path = fmt::format("{:08X}", uint32_t(content_type));

  rex::filesystem::WildcardEngine title_find_engine;
  title_find_engine.SetRule("????????");

  rex::filesystem::WildcardEngine content_find_engine;
  content_find_engine.SetRule("????????????????");

  size_t title_find_index = 0;
  rex::filesystem::Entry* title_entry;
  for (;;) {
    title_entry = root_entry->IterateChildren(title_find_engine, &title_find_index);
    if (!title_entry) {
      break;
    }

    auto title_id = rex::string::from_string<uint32_t>(title_entry->name(), true);

    auto content_root_entry = title_entry->ResolvePath(content_type_path);
    if (content_root_entry) {
      size_t content_find_index = 0;
      rex::filesystem::Entry* content_entry;
      for (;;) {
        content_entry =
            content_root_entry->IterateChildren(content_find_engine, &content_find_index);
        if (!content_entry) {
          break;
        }

        auto item = e->AppendItem();
        assert_not_null(item);
        if (item) {
          item->device_id = static_cast<uint32_t>(DummyDeviceId::ODD);
          item->content_type = content_type;
          item->set_display_name(string::to_utf16(content_entry->name()));
          item->set_file_name(content_entry->name());
          item->title_id = title_id;
        }
      }
    }
  }
}

u32 XamContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id, u32 content_type, u32 unk3,
                                              mapped_u32 handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    return X_E_INVALIDARG;
  }

  auto e = make_object<XStaticEnumerator<XCONTENT_AGGREGATE_DATA>>(REX_KERNEL_STATE(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(0xFF, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }

  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  auto content_type_enum = XContentType(uint32_t(content_type));

  uint64_t userxuid = REX_KERNEL_STATE()->user_profile()->xuid();

  if (!device_info || device_info->device_type == DeviceType::HDD) {
    // Fetch any alternate title IDs defined in the XEX header
    // (used by games to load saves from other titles, etc)
    std::vector<uint32_t> title_ids{kCurrentlyRunningTitleId};
    auto exe_module = REX_KERNEL_STATE()->GetExecutableModule();
    if (exe_module && exe_module->xex_module()) {
      const auto& alt_ids = exe_module->xex_module()->opt_alternate_title_ids();
      std::copy(alt_ids.cbegin(), alt_ids.cend(), std::back_inserter(title_ids));
    }

    for (auto& title_id : title_ids) {
      // Get user-specific content
      auto content_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD), xuid, content_type_enum, title_id);
      for (const auto& content_data : content_datas) {
        auto item = e->AppendItem();
        assert_not_null(item);
        if (item) {
          *item = content_data;
        }
      }

      // Also get common content (xuid=0)
      if (userxuid != 0) {
        auto common_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
            static_cast<uint32_t>(DummyDeviceId::HDD), 0, content_type_enum, title_id);
        for (const auto& content_data : common_datas) {
          auto item = e->AppendItem();
          assert_not_null(item);
          if (item) {
            *item = content_data;
          }
        }
      }
    }
  }

  if (!device_info || device_info->device_type == DeviceType::ODD) {
    AddODDContentTest(e, content_type_enum);
  }

  REXKRNL_DEBUG("XamContentAggregateCreateEnumerator: added {} items to enumerator",
                e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamContentAggregateCreateEnumerator,
           rex::kernel::xam::XamContentAggregateCreateEnumerator_entry)
