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
#include <cstring>
#include <vector>

#include <rex/kernel/types/content.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/title_runtime.h>
#include "xam/xam_ui.h"
#include <rex/logging/log.h>
#include "xam/categories.h"
#include <rex/math.h>
#include <rex/ppc/hook.h>
#include <rex/xam/content_manager.h>
#include <rex/xam/device.h>
#include <rex/xam/user_profile.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/types/status.h>
#include <rex/types.h>
#include "xam/enumerator_object.h"
#include "xam/stub_log.h"
#include "xam/xam_module.h"
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

u32 XamContentGetLicenseMask_entry(MappedU32 mask_ptr, MappedVoid overlapped_ptr) {
  // Bits of the licenses applicable to the title, combined with OR.
  *mask_ptr = GetXAMSettings().license_mask->Get();

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = kernel::X_ERROR_SUCCESS});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return kernel::X_ERROR_SUCCESS;
  }
}

u32 XamContentResolve_entry([[maybe_unused]] u32 user_index,
                            [[maybe_unused]] MappedVoid content_data_ptr,
                            [[maybe_unused]] MappedVoid buffer_ptr,
                            [[maybe_unused]] u32 buffer_size, [[maybe_unused]] u32 unk1,
                            [[maybe_unused]] u32 unk2, [[maybe_unused]] u32 unk3) {
  REX_XAM_STUB("XamContentResolve");
  return kernel::X_ERROR_NOT_FOUND;
}

u32 XamContentCreateEnumerator_entry([[maybe_unused]] u32 user_index, u32 device_id,
                                     u32 content_type, [[maybe_unused]] u32 content_flags,
                                     u32 items_per_enumerate, MappedU32 buffer_size_ptr,
                                     MappedU32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  auto device_info = device_id == 0 ? nullptr : rex::xam::GetPlaceholderDevice(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return kernel::X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(kernel::XCONTENT_DATA) * items_per_enumerate;
  }

  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();

  StaticItemList items(items_per_enumerate, sizeof(kernel::XCONTENT_DATA));

  if (!device_info || device_info->device_id == rex::xam::DummyDeviceID::kHDD) {
    auto contents = CurrentXAM().GetContentManager().ListContent(
        std::to_underlying(rex::xam::DummyDeviceID::kHDD), xuid,
        static_cast<rex::kernel::XContentType>(content_type));
    for (const auto& content_data : contents) {
      auto item = items.Append<kernel::XCONTENT_DATA>();
      *item = content_data;
    }

    if (xuid != 0) {
      auto common_datas = CurrentXAM().GetContentManager().ListContent(
          std::to_underlying(rex::xam::DummyDeviceID::kHDD), 0,
          static_cast<rex::kernel::XContentType>(content_type));
      for (const auto& content_data : common_datas) {
        auto item = items.Append<kernel::XCONTENT_DATA>();
        *item = content_data;
      }
    }
  }

  if (!device_info || device_info->device_id == rex::xam::DummyDeviceID::kODD) {}

  REXXAM_DEBUG("XamContentCreateEnumerator: added {} items to enumerator", items.GetItemCount());

  kernel::X_HANDLE handle = kernel::kXInvalidHandleValue;
  auto result = CreateEnumerator(
      &handle, EnumeratorInit{0xFF, 0xFE, 0x20005, 0x20007, 0},
      EnumeratorObject(
          items_per_enumerate, sizeof(kernel::XCONTENT_DATA),
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

enum class DispositionState : u8 { kUnknown = 0, kCreate = 1, kOpen = 2 };

u32 CreateAndMountContent(MappedString root_name, MappedVoid content_data_ptr,
                          u32 content_data_size, u32 flags, MappedU32 disposition_ptr,
                          MappedU32 license_mask_ptr, MappedVoid overlapped_ptr) {
  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();

  kernel::XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(kernel::XCONTENT_DATA)) {
    content_data = *content_data_ptr.As<kernel::XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(kernel::XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.As<kernel::XCONTENT_AGGREGATE_DATA*>();
  } else {
    REX_ASSERT_ALWAYS();
    return kernel::X_ERROR_INVALID_PARAMETER;
  }

  if (content_data.content_type == rex::kernel::XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto* content_manager = &CurrentXAM().GetContentManager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, root_name = root_name.GetValue(), flags, content_data,
              disposition_ptr,
              license_mask_ptr](u32& extended_error, u32& length) -> kernel::X_RESULT {
    kernel::X_RESULT result = kernel::X_ERROR_INVALID_PARAMETER;
    DispositionState disposition = DispositionState::kUnknown;
    switch (flags & 0xF) {
      // CREATE_NEW
      case 1:
        if (content_manager->HasContent(xuid, content_data)) {
          result = kernel::X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = DispositionState::kCreate;
        }
        break;
      // CREATE_ALWAYS
      case 2:
        // Titles may reuse a root name without closing it, so close the mount first.
        if (content_manager->CloseContent(root_name)) {
          REXXAM_DEBUG("XamContentCreate: closed the open {} before recreating it", root_name);
        }
        if (content_manager->HasContent(xuid, content_data) &&
            !content_manager->DeleteContent(xuid, content_data)) {
          result = kernel::X_ERROR_ACCESS_DENIED;
          break;
        }
        disposition = DispositionState::kCreate;
        break;
      // OPEN_EXISTING
      case 3:
        if (!content_manager->HasContent(xuid, content_data)) {
          result = kernel::X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = DispositionState::kOpen;
        }
        break;
      // OPEN_ALWAYS
      case 4:
        if (!content_manager->HasContent(xuid, content_data)) {
          disposition = DispositionState::kCreate;
        } else {
          disposition = DispositionState::kOpen;
        }
        break;
      // TRUNCATE_EXISTING
      case 5:
        if (!content_manager->HasContent(xuid, content_data)) {
          result = kernel::X_ERROR_PATH_NOT_FOUND;
        } else {
          if (content_manager->CloseContent(root_name)) {
            REXXAM_DEBUG("XamContentCreate: closed the open {} before truncating it", root_name);
          }
          if (!content_manager->DeleteContent(xuid, content_data)) {
            result = kernel::X_ERROR_ACCESS_DENIED;
          } else {
            disposition = DispositionState::kCreate;
          }
        }
        break;
      default:
        REX_UNHANDLED_CASE(flags & 0xF);
        break;
    }

    u32 content_license = 0;
    if (disposition == DispositionState::kCreate) {
      result = kernel::ToXResult(content_manager->CreateContent(root_name, xuid, content_data));
      if (result == kernel::X_ERROR_SUCCESS) {
        if (auto written = content_manager->WriteContentHeaderFile(xuid, content_data); !written) {
          REXXAM_WARN("XamContentCreate: header file of {} not written, {}", root_name,
                      written.error().message);
          result = kernel::ToXResult(written);
          if (auto closed = content_manager->CloseContent(root_name); !closed) {
            REXXAM_WARN("XamContentCreate: {} did not close, {}", root_name,
                        closed.error().message);
          }
          if (auto deleted = content_manager->DeleteContent(xuid, content_data); !deleted) {
            REXXAM_WARN("XamContentCreate: {} was not removed, {}", root_name,
                        deleted.error().message);
          }
        }
      }
    } else if (disposition == DispositionState::kOpen) {
      auto opened = content_manager->OpenContent(root_name, xuid, content_data);
      if (opened) {
        content_license = *opened;
      }
      result = kernel::ToXResult(opened);
    }

    if (license_mask_ptr && result == kernel::X_ERROR_SUCCESS) {
      *license_mask_ptr = content_license;
    }

    if (disposition_ptr) {
      *disposition_ptr = static_cast<u32>(disposition);
    }

    extended_error = kernel::XHRESULTFromWin32(result);
    length = static_cast<u32>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    u32 extended_error, length;
    return run(extended_error, length);
  } else {
    CurrentTitle().overlapped_dispatcher.CompleteDeferred(
        overlapped_ptr.GetGuestAddress(), [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          const kernel::X_RESULT result = run(extended_error, length);
          return kernel::CompletionOptions{
              .result = result, .extended_error = extended_error, .length = length};
        });
    return kernel::X_ERROR_IO_PENDING;
  }
}

u32 XamContentCreateEx_entry([[maybe_unused]] u32 user_index, MappedString root_name,
                             MappedVoid content_data_ptr, u32 flags, MappedU32 disposition_ptr,
                             MappedU32 license_mask_ptr, [[maybe_unused]] u32 cache_size,
                             [[maybe_unused]] u64 content_size, MappedVoid overlapped_ptr) {
  return CreateAndMountContent(root_name, content_data_ptr, sizeof(kernel::XCONTENT_DATA), flags,
                               disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreate_entry([[maybe_unused]] u32 user_index, MappedString root_name,
                           MappedVoid content_data_ptr, u32 flags, MappedU32 disposition_ptr,
                           MappedU32 license_mask_ptr, MappedVoid overlapped_ptr) {
  return CreateAndMountContent(root_name, content_data_ptr, sizeof(kernel::XCONTENT_DATA), flags,
                               disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreateInternal_entry(MappedString root_name, MappedVoid content_data_ptr, u32 flags,
                                   MappedU32 disposition_ptr, MappedU32 license_mask_ptr,
                                   [[maybe_unused]] u32 cache_size,
                                   [[maybe_unused]] u64 content_size, MappedVoid overlapped_ptr) {
  return CreateAndMountContent(root_name, content_data_ptr, sizeof(kernel::XCONTENT_AGGREGATE_DATA),
                               flags, disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentOpenFile_entry([[maybe_unused]] u32 user_index,
                             [[maybe_unused]] MappedString root_name,
                             [[maybe_unused]] MappedString path, [[maybe_unused]] u32 flags,
                             [[maybe_unused]] MappedU32 disposition_ptr,
                             [[maybe_unused]] MappedU32 license_mask_ptr,
                             [[maybe_unused]] MappedVoid overlapped_ptr) {
  return kernel::X_ERROR_FILE_NOT_FOUND;
}

u32 XamContentFlush_entry([[maybe_unused]] MappedString root_name, MappedVoid overlapped_ptr) {
  kernel::X_RESULT result = kernel::X_ERROR_SUCCESS;
  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentClose_entry(MappedString root_name, MappedVoid overlapped_ptr) {
  auto result =
      kernel::ToXResult(CurrentXAM().GetContentManager().CloseContent(root_name.GetValue()));

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetCreator_entry([[maybe_unused]] u32 user_index, MappedVoid content_data_ptr,
                               MappedU32 is_creator_ptr, MappedU64 creator_xuid_ptr,
                               MappedVoid overlapped_ptr) {
  auto result = kernel::X_ERROR_SUCCESS;

  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();
  kernel::XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.As<kernel::XCONTENT_DATA*>();

  bool content_exists = CurrentXAM().GetContentManager().HasContent(xuid, content_data);

  if (content_exists) {
    if (content_data.content_type == rex::kernel::XContentType::kSavedGame) {
      *is_creator_ptr = 1;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = xuid;
      }
    } else {
      *is_creator_ptr = 0;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = 0;
      }
    }
  } else {
    result = kernel::X_ERROR_PATH_NOT_FOUND;
  }

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetThumbnail_entry([[maybe_unused]] u32 user_index, MappedVoid content_data_ptr,
                                 MappedVoid buffer_ptr, MappedU32 buffer_size_ptr,
                                 MappedVoid overlapped_ptr) {
  REX_ASSERT(buffer_size_ptr != nullptr);
  u32 buffer_size = *buffer_size_ptr;
  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();
  kernel::XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.As<kernel::XCONTENT_DATA*>();

  std::vector<u8> buffer;
  auto thumbnail = CurrentXAM().GetContentManager().GetContentThumbnail(xuid, content_data);
  if (thumbnail) {
    buffer = std::move(*thumbnail);
  }
  auto result = kernel::ToXResult(thumbnail);

  *buffer_size_ptr = static_cast<u32>(buffer.size());

  if (result == kernel::X_ERROR_SUCCESS) {
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        result = kernel::X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        std::memcpy(static_cast<u8*>(buffer_ptr), buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentSetThumbnail_entry([[maybe_unused]] u32 user_index, MappedVoid content_data_ptr,
                                 MappedVoid buffer_ptr, u32 buffer_size,
                                 MappedVoid overlapped_ptr) {
  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();
  kernel::XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.As<kernel::XCONTENT_DATA*>();

  // Buffer is PNG data.
  auto buffer =
      std::vector<u8>(static_cast<u8*>(buffer_ptr), static_cast<u8*>(buffer_ptr) + buffer_size);
  auto result = kernel::ToXResult(
      CurrentXAM().GetContentManager().SetContentThumbnail(xuid, content_data, std::move(buffer)));

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDelete_entry([[maybe_unused]] u32 user_index, MappedVoid content_data_ptr,
                           MappedVoid overlapped_ptr) {
  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();
  kernel::XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.As<kernel::XCONTENT_DATA*>();

  auto result =
      kernel::ToXResult(CurrentXAM().GetContentManager().DeleteContent(xuid, content_data));

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDeleteInternal_entry(MappedVoid content_data_ptr, MappedVoid overlapped_ptr) {
  // Internal variants wrap the public call with 0xFE as the user index.
  u64 xuid = CurrentXAM().GetUserProfile().GetXUID();
  kernel::XCONTENT_AGGREGATE_DATA content_data =
      *content_data_ptr.As<kernel::XCONTENT_AGGREGATE_DATA*>();

  auto result =
      kernel::ToXResult(CurrentXAM().GetContentManager().DeleteContent(xuid, content_data));

  if (overlapped_ptr) {
    CurrentTitle().overlapped_dispatcher.Complete(overlapped_ptr.GetGuestAddress(),
                                                  {.result = result});
    return kernel::X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamContentGetLicenseMask, rex::xam::XamContentGetLicenseMask_entry)
REX_XAM_BODY(__imp__XamContentResolve, rex::xam::XamContentResolve_entry)
REX_XAM_BODY(__imp__XamContentCreateEnumerator, rex::xam::XamContentCreateEnumerator_entry)
REX_XAM_BODY(__imp__XamContentCreateEx, rex::xam::XamContentCreateEx_entry)
REX_XAM_BODY(__imp__XamContentCreate, rex::xam::XamContentCreate_entry)
REX_XAM_BODY(__imp__XamContentCreateInternal, rex::xam::XamContentCreateInternal_entry)
REX_XAM_BODY(__imp__XamContentOpenFile, rex::xam::XamContentOpenFile_entry)
REX_XAM_BODY(__imp__XamContentFlush, rex::xam::XamContentFlush_entry)
REX_XAM_BODY(__imp__XamContentClose, rex::xam::XamContentClose_entry)
REX_XAM_BODY(__imp__XamContentGetCreator, rex::xam::XamContentGetCreator_entry)
REX_XAM_BODY(__imp__XamContentGetThumbnail, rex::xam::XamContentGetThumbnail_entry)
REX_XAM_BODY(__imp__XamContentSetThumbnail, rex::xam::XamContentSetThumbnail_entry)
REX_XAM_BODY(__imp__XamContentDelete, rex::xam::XamContentDelete_entry)
REX_XAM_BODY(__imp__XamContentDeleteInternal, rex::xam::XamContentDeleteInternal_entry)
