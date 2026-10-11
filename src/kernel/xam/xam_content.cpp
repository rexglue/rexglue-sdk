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

#include <rex/api.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/runtime.h>
#include <rex/runtime/xam/content_device.h>
#include <rex/runtime/enumerator_object.h>
#include <rex/runtime/xtypes.h>
#include "kernel/kernel_macros.h"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::runtime;

u32 XamContentGetLicenseMask_entry(mapped_u32 mask_ptr, mapped_void overlapped_ptr) {
  // Bits of the licenses applicable to the title, combined with OR.
  *mask_ptr = runtime::Current().config().runtime.kernel.license_mask;

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(),
                                             {.result = X_ERROR_SUCCESS});
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}

u32 XamContentResolve_entry([[maybe_unused]] u32 user_index,
                            [[maybe_unused]] mapped_void content_data_ptr,
                            [[maybe_unused]] mapped_void buffer_ptr,
                            [[maybe_unused]] u32 buffer_size, [[maybe_unused]] u32 unk1,
                            [[maybe_unused]] u32 unk2, [[maybe_unused]] u32 unk3) {
  REX_ASSERT_ALWAYS();
  REX_KERNEL_STUB("XamContentResolve");
  return X_ERROR_NOT_FOUND;
}

u32 XamContentCreateEnumerator_entry([[maybe_unused]] u32 user_index, u32 device_id,
                                     u32 content_type, [[maybe_unused]] u32 content_flags,
                                     u32 items_per_enumerate, mapped_u32 buffer_size_ptr,
                                     mapped_u32 handle_out) {
  REX_ASSERT(handle_out != nullptr);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  u64 xuid = runtime::Current().content_manager().user_profile().xuid();

  StaticItems items(items_per_enumerate, sizeof(XCONTENT_DATA));

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    auto content_datas = runtime::Current().content_manager().ListContent(
        std::to_underlying(DummyDeviceId::HDD), xuid, XContentType(content_type));
    for (const auto& content_data : content_datas) {
      auto item = items.Append<XCONTENT_DATA>();
      *item = content_data;
    }

    if (xuid != 0) {
      auto common_datas = runtime::Current().content_manager().ListContent(
          std::to_underlying(DummyDeviceId::HDD), 0, XContentType(content_type));
      for (const auto& content_data : common_datas) {
        auto item = items.Append<XCONTENT_DATA>();
        *item = content_data;
      }
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {}

  REXLOG_DEBUG(kernel, "XamContentCreateEnumerator: added {} items to enumerator",
               items.item_count());

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  auto result = CreateEnumerator(
      runtime::Current(), &handle, EnumeratorInit{0xFF, 0xFE, 0x20005, 0x20007, 0},
      EnumeratorObject(
          items_per_enumerate, sizeof(XCONTENT_DATA),
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

enum class kDispositionState : u8 { Unknown = 0, Create = 1, Open = 2 };

u32 XamContentCreate(mapped_string root_name, mapped_void content_data_ptr, u32 content_data_size,
                     u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                     mapped_void overlapped_ptr) {
  u64 xuid = runtime::Current().content_manager().user_profile().xuid();

  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    REX_ASSERT_ALWAYS();
    return X_ERROR_INVALID_PARAMETER;
  }

  if (content_data.content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto* content_manager = &runtime::Current().content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, root_name = root_name.value(), flags, content_data,
              disposition_ptr, license_mask_ptr](u32& extended_error, u32& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags & 0xF) {
      // CREATE_NEW
      case 1:
        if (content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      // CREATE_ALWAYS
      case 2:
        // Titles may reuse a root name without closing it, so close the mount first.
        if (content_manager->CloseContent(root_name)) {
          REXLOG_DEBUG(kernel, "XamContentCreate: closed the open {} before recreating it",
                       root_name);
        }
        if (content_manager->ContentExists(xuid, content_data) &&
            !content_manager->DeleteContent(xuid, content_data)) {
          result = X_ERROR_ACCESS_DENIED;
          break;
        }
        disposition = kDispositionState::Create;
        break;
      // OPEN_EXISTING
      case 3:
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      // OPEN_ALWAYS
      case 4:
        if (!content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Create;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      // TRUNCATE_EXISTING
      case 5:
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          if (content_manager->CloseContent(root_name)) {
            REXLOG_DEBUG(kernel, "XamContentCreate: closed the open {} before truncating it",
                         root_name);
          }
          if (!content_manager->DeleteContent(xuid, content_data)) {
            result = X_ERROR_ACCESS_DENIED;
          } else {
            disposition = kDispositionState::Create;
          }
        }
        break;
      default:
        REX_UNHANDLED_CASE(flags & 0xF);
        break;
    }

    u32 content_license = 0;
    if (disposition == kDispositionState::Create) {
      result = ToXResult(content_manager->CreateContent(root_name, xuid, content_data));
      if (result == X_ERROR_SUCCESS) {
        if (auto written = content_manager->WriteContentHeaderFile(xuid, content_data); !written) {
          REXLOG_WARN(kernel, "XamContentCreate: header file of {} not written, {}", root_name,
                      written.error().message);
          result = ToXResult(written);
          if (auto closed = content_manager->CloseContent(root_name); !closed) {
            REXLOG_WARN(kernel, "XamContentCreate: {} did not close, {}", root_name,
                        closed.error().message);
          }
          if (auto deleted = content_manager->DeleteContent(xuid, content_data); !deleted) {
            REXLOG_WARN(kernel, "XamContentCreate: {} was not removed, {}", root_name,
                        deleted.error().message);
          }
        }
      }
    } else if (disposition == kDispositionState::Open) {
      auto opened = content_manager->OpenContent(root_name, xuid, content_data);
      if (opened) {
        content_license = *opened;
      }
      result = ToXResult(opened);
    }

    if (license_mask_ptr && result == X_ERROR_SUCCESS) {
      *license_mask_ptr = content_license;
    }

    if (disposition_ptr) {
      *disposition_ptr = static_cast<u32>(disposition);
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<u32>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    u32 extended_error, length;
    return run(extended_error, length);
  } else {
    runtime::Current().dispatcher().CompleteDeferred(
        overlapped_ptr.guest_address(), [run = std::move(run)]() mutable {
          u32 extended_error = 0;
          u32 length = 0;
          const X_RESULT result = run(extended_error, length);
          return runtime::CompletionOptions{
              .result = result, .extended_error = extended_error, .length = length};
        });
    return X_ERROR_IO_PENDING;
  }
}

u32 XamContentCreateEx_entry([[maybe_unused]] u32 user_index, mapped_string root_name,
                             mapped_void content_data_ptr, u32 flags, mapped_u32 disposition_ptr,
                             mapped_u32 license_mask_ptr, [[maybe_unused]] u32 cache_size,
                             [[maybe_unused]] u64 content_size, mapped_void overlapped_ptr) {
  return XamContentCreate(root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                          disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreate_entry([[maybe_unused]] u32 user_index, mapped_string root_name,
                           mapped_void content_data_ptr, u32 flags, mapped_u32 disposition_ptr,
                           mapped_u32 license_mask_ptr, mapped_void overlapped_ptr) {
  return XamContentCreate(root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                          disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentCreateInternal_entry(mapped_string root_name, mapped_void content_data_ptr, u32 flags,
                                   mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                   [[maybe_unused]] u32 cache_size,
                                   [[maybe_unused]] u64 content_size, mapped_void overlapped_ptr) {
  return XamContentCreate(root_name, content_data_ptr, sizeof(XCONTENT_AGGREGATE_DATA), flags,
                          disposition_ptr, license_mask_ptr, overlapped_ptr);
}

u32 XamContentOpenFile_entry([[maybe_unused]] u32 user_index,
                             [[maybe_unused]] mapped_string root_name,
                             [[maybe_unused]] mapped_string path, [[maybe_unused]] u32 flags,
                             [[maybe_unused]] mapped_u32 disposition_ptr,
                             [[maybe_unused]] mapped_u32 license_mask_ptr,
                             [[maybe_unused]] mapped_void overlapped_ptr) {
  return X_ERROR_FILE_NOT_FOUND;
}

u32 XamContentFlush_entry([[maybe_unused]] mapped_string root_name, mapped_void overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentClose_entry(mapped_string root_name, mapped_void overlapped_ptr) {
  auto result = ToXResult(runtime::Current().content_manager().CloseContent(root_name.value()));

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetCreator_entry([[maybe_unused]] u32 user_index, mapped_void content_data_ptr,
                               mapped_u32 is_creator_ptr, mapped_u64 creator_xuid_ptr,
                               mapped_void overlapped_ptr) {
  auto result = X_ERROR_SUCCESS;

  u64 xuid = runtime::Current().content_manager().user_profile().xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  bool content_exists = runtime::Current().content_manager().ContentExists(xuid, content_data);

  if (content_exists) {
    if (content_data.content_type == XContentType::kSavedGame) {
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
    result = X_ERROR_PATH_NOT_FOUND;
  }

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetThumbnail_entry([[maybe_unused]] u32 user_index, mapped_void content_data_ptr,
                                 mapped_void buffer_ptr, mapped_u32 buffer_size_ptr,
                                 mapped_void overlapped_ptr) {
  REX_ASSERT(buffer_size_ptr != nullptr);
  u32 buffer_size = *buffer_size_ptr;
  u64 xuid = runtime::Current().content_manager().user_profile().xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  std::vector<u8> buffer;
  auto thumbnail = runtime::Current().content_manager().GetContentThumbnail(xuid, content_data);
  if (thumbnail) {
    buffer = std::move(*thumbnail);
  }
  auto result = ToXResult(thumbnail);

  *buffer_size_ptr = static_cast<u32>(buffer.size());

  if (result == X_ERROR_SUCCESS) {
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        result = X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        std::memcpy(static_cast<u8*>(buffer_ptr), buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentSetThumbnail_entry([[maybe_unused]] u32 user_index, mapped_void content_data_ptr,
                                 mapped_void buffer_ptr, u32 buffer_size,
                                 mapped_void overlapped_ptr) {
  u64 xuid = runtime::Current().content_manager().user_profile().xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Buffer is PNG data.
  auto buffer =
      std::vector<u8>(static_cast<u8*>(buffer_ptr), static_cast<u8*>(buffer_ptr) + buffer_size);
  auto result = ToXResult(runtime::Current().content_manager().SetContentThumbnail(
      xuid, content_data, std::move(buffer)));

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDelete_entry([[maybe_unused]] u32 user_index, mapped_void content_data_ptr,
                           mapped_void overlapped_ptr) {
  u64 xuid = runtime::Current().content_manager().user_profile().xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto result = ToXResult(runtime::Current().content_manager().DeleteContent(xuid, content_data));

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDeleteInternal_entry(mapped_void content_data_ptr, mapped_void overlapped_ptr) {
  // Internal variants wrap the public call with 0xFE as the user index.
  u64 xuid = runtime::Current().content_manager().user_profile().xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();

  auto result = ToXResult(runtime::Current().content_manager().DeleteContent(xuid, content_data));

  if (overlapped_ptr) {
    runtime::Current().dispatcher().Complete(overlapped_ptr.guest_address(), {.result = result});
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamContentGetLicenseMask, rex::kernel::xam::XamContentGetLicenseMask_entry)
REX_EXPORT(__imp__XamContentResolve, rex::kernel::xam::XamContentResolve_entry)
REX_EXPORT(__imp__XamContentCreateEnumerator, rex::kernel::xam::XamContentCreateEnumerator_entry)
REX_EXPORT(__imp__XamContentCreateEx, rex::kernel::xam::XamContentCreateEx_entry)
REX_EXPORT(__imp__XamContentCreate, rex::kernel::xam::XamContentCreate_entry)
REX_EXPORT(__imp__XamContentCreateInternal, rex::kernel::xam::XamContentCreateInternal_entry)
REX_EXPORT(__imp__XamContentOpenFile, rex::kernel::xam::XamContentOpenFile_entry)
REX_EXPORT(__imp__XamContentFlush, rex::kernel::xam::XamContentFlush_entry)
REX_EXPORT(__imp__XamContentClose, rex::kernel::xam::XamContentClose_entry)
REX_EXPORT(__imp__XamContentGetCreator, rex::kernel::xam::XamContentGetCreator_entry)
REX_EXPORT(__imp__XamContentGetThumbnail, rex::kernel::xam::XamContentGetThumbnail_entry)
REX_EXPORT(__imp__XamContentSetThumbnail, rex::kernel::xam::XamContentSetThumbnail_entry)
REX_EXPORT(__imp__XamContentDelete, rex::kernel::xam::XamContentDelete_entry)
REX_EXPORT(__imp__XamContentDeleteInternal, rex::kernel::xam::XamContentDeleteInternal_entry)

REX_EXPORT_STUB(__imp__XamContentClosePackageFile);
REX_EXPORT_STUB(__imp__XamContentCopyInternal);
REX_EXPORT_STUB(__imp__XamContentCreateAndMountPackage);
REX_EXPORT_STUB(__imp__XamContentCreateEnumeratorInternal);
REX_EXPORT_STUB(__imp__XamContentDismountAndClosePackage);
REX_EXPORT_STUB(__imp__XamContentDuplicateFileHandle);
REX_EXPORT_STUB(__imp__XamContentExistsOnDeviceInternal);
REX_EXPORT_STUB(__imp__XamContentFlushPackage);
REX_EXPORT_STUB(__imp__XamContentGetAttributes);
REX_EXPORT_STUB(__imp__XamContentGetAttributesInternal);
REX_EXPORT_STUB(__imp__XamContentGetHeaderInternal);
REX_EXPORT_STUB(__imp__XamContentGetLocalizedString);
REX_EXPORT_STUB(__imp__XamContentGetMetaDataInternal);
REX_EXPORT_STUB(__imp__XamContentGetMountedPackageByRootName);
REX_EXPORT_STUB(__imp__XamContentGetOnlineCreator);
REX_EXPORT_STUB(__imp__XamContentInstall);
REX_EXPORT_STUB(__imp__XamContentInstallInternal);
REX_EXPORT_STUB(__imp__XamContentIsGameInstalledToHDD);
REX_EXPORT_STUB(__imp__XamContentLaunchImage);
REX_EXPORT_STUB(__imp__XamContentLaunchImageFromFileInternal);
REX_EXPORT_STUB(__imp__XamContentLaunchImageInternal);
REX_EXPORT_STUB(__imp__XamContentLaunchImageInternalEx);
REX_EXPORT_STUB(__imp__XamContentLockUnlockPackageHeaders);
REX_EXPORT_STUB(__imp__XamContentMountInstalledGame);
REX_EXPORT_STUB(__imp__XamContentMountPackage);
REX_EXPORT_STUB(__imp__XamContentMoveInternal);
REX_EXPORT_STUB(__imp__XamContentOpenFileInternal);
REX_EXPORT_STUB(__imp__XamContentOpenPackageFile);
REX_EXPORT_STUB(__imp__XamContentQueryLicenseInternal);
REX_EXPORT_STUB(__imp__XamContentRegisterChangeCallback);
REX_EXPORT_STUB(__imp__XamContentResolveInternal);
REX_EXPORT_STUB(__imp__XamContentSetAttributes);
REX_EXPORT_STUB(__imp__XamContentSetMediaMetaDataInternal);
REX_EXPORT_STUB(__imp__XamContentSetThumbnailInternal);
REX_EXPORT_STUB(__imp__XamContentWritePackageHeader);
