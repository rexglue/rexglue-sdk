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
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/thread.h>
#include <rex/ppc/call.h>
#include "xam/xam_ui.h"
#include <rex/logging/log_category.h>
#include "xam/categories.h"
#include <rex/ppc/hook.h>
#include <rex/memory/mapped_ptr.h>
#include <rex/kernel/module.h>
#include <rex/types.h>
#include <rex/string/text.h>
#include <rex/filesystem/path.h>
#include <rex/kernel/title_runtime.h>
#include <rex/kernel/object/module_object.h>
#include <rex/kernel/guest_allocator.h>
#include "xam/enumerator_object.h"
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/types/status.h>
#include "xam/stub_log.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include <ctime>
#include <format>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/types/xex.h>
#include <rex/kernel/object/object_table.h>
#include "xam/xam_bodies.h"

namespace rex {
namespace xam {

using namespace rex::kernel;

u32 XamFeatureEnabled_entry([[maybe_unused]] u32 unk) {
  return 0;
}

u8 g_schema_bin[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
};

struct XAMSchema {
  be_u32 data_ptr;
  be_u32 size;
};
REX_STATIC_ASSERT_SIZE(XAMSchema, 8);

u32 XamGetOnlineSchema_entry() {
  rex::xam::XAMState& xam = CurrentXAM();
  u32 schema_address = xam.GetOnlineSchemaAddress();

  if (!schema_address) {
    schema_address = CurrentTitle().heap.SystemHeapAlloc(sizeof(XAMSchema) + sizeof(g_schema_bin));
    xam.SetOnlineSchemaAddress(schema_address);
    auto* info = CurrentTitle().membase.TranslateVirtual<XAMSchema*>(schema_address);
    std::memcpy(info + 1, g_schema_bin, sizeof(g_schema_bin));
    info->data_ptr = schema_address + sizeof(XAMSchema);
    info->size = static_cast<u32>(sizeof(g_schema_bin));
  }

  return schema_address;
}

struct LocalDateTime {
  u32 year;
  u32 month;
  u32 day;
  u32 hour;
  u32 minute;
};

static LocalDateTime ToLocalDateTime(u64 filetime) {
#if REX_PLATFORM_WIN32
  FILETIME t;
  t.dwHighDateTime = filetime >> 32;
  t.dwLowDateTime = static_cast<u32>(filetime);

  SYSTEMTIME st = {};
  SYSTEMTIME local_st = {};
  FileTimeToSystemTime(&t, &st);
  SystemTimeToTzSpecificLocalTime(nullptr, &st, &local_st);
  return {local_st.wYear, local_st.wMonth, local_st.wDay, local_st.wHour, local_st.wMinute};
#else
  constexpr i64 kUnixEpochFiletime = 116444736000000000;
  constexpr i64 kFiletimeTicksPerSecond = 10000000;
  const i64 ticks = static_cast<i64>(filetime) - kUnixEpochFiletime;
  i64 seconds = ticks / kFiletimeTicksPerSecond;
  if (ticks % kFiletimeTicksPerSecond < 0) {
    --seconds;
  }
  const auto time = static_cast<std::time_t>(seconds);
  std::tm local = {};
  localtime_r(&time, &local);
  return {static_cast<u32>(local.tm_year + 1900), static_cast<u32>(local.tm_mon + 1),
          static_cast<u32>(local.tm_mday), static_cast<u32>(local.tm_hour),
          static_cast<u32>(local.tm_min)};
#endif
}

void XamFormatDateString_entry([[maybe_unused]] u32 unk, u64 filetime, MappedVoid output_buffer,
                               u32 output_count) {
  std::memset(output_buffer, 0, output_count * sizeof(char16_t));

  const LocalDateTime local = ToLocalDateTime(filetime);
  auto str =
      rex::string::ToUTF16(std::format("{:02d}/{:02d}/{}", local.month, local.day, local.year));
  rex::string::CopyAndSwapTruncating(output_buffer.As<char16_t*>(), str, output_count);
}

void XamFormatTimeString_entry([[maybe_unused]] u32 unk, u64 filetime, MappedVoid output_buffer,
                               u32 output_count) {
  std::memset(output_buffer, 0, output_count * sizeof(char16_t));

  const LocalDateTime local = ToLocalDateTime(filetime);
  auto str = rex::string::ToUTF16(std::format("{:02d}:{:02d}", local.hour, local.minute));
  rex::string::CopyAndSwapTruncating(output_buffer.As<char16_t*>(), str, output_count);
}

u32 BuildResourceLocator(u64 module, std::u16string_view container, std::u16string_view resource,
                         MappedVoid buffer_ptr, u32 buffer_count) {
  std::u16string path;
  if (!module) {
    path = rex::string::ToUTF16(std::format(
        "file://media:/{}.xzp#{}", rex::string::ToUTF8(container), rex::string::ToUTF8(resource)));
    REXXAM_DEBUG("XamBuildResourceLocator({0}) returning locator to local file {0}.xzp",
                 rex::string::ToUTF8(container));
  } else {
    path = rex::string::ToUTF16(std::format("section://{:X},{}#{}", static_cast<u32>(module),
                                            rex::string::ToUTF8(container),
                                            rex::string::ToUTF8(resource)));
  }
  rex::string::CopyAndSwapTruncating(buffer_ptr.As<char16_t*>(), path, buffer_count);
  return 0;
}

u32 XamBuildResourceLocator_entry(u64 module, MappedWString container, MappedWString resource,
                                  MappedVoid buffer_ptr, u32 buffer_count) {
  return BuildResourceLocator(module, container.GetValue(), resource.GetValue(), buffer_ptr,
                              buffer_count);
}

u32 XamBuildGamercardResourceLocator_entry(MappedWString filename, MappedVoid buffer_ptr,
                                           u32 buffer_count) {
  // Resources resolve to local .xzp files extracted from xam.xex.

  return BuildResourceLocator(0, u"gamercrd", filename.GetValue(), buffer_ptr, buffer_count);
}

u32 XamBuildSharedSystemResourceLocator_entry(MappedWString filename, MappedVoid buffer_ptr,
                                              u32 buffer_count) {
  return BuildResourceLocator(0, u"shrdres", filename.GetValue(), buffer_ptr, buffer_count);
}

u32 XamBuildLegacySystemResourceLocator_entry(MappedWString filename, MappedVoid buffer_ptr,
                                              u32 buffer_count) {
  return XamBuildSharedSystemResourceLocator_entry(filename, buffer_ptr, buffer_count);
}

u32 XamBuildXamResourceLocator_entry(MappedWString filename, MappedVoid buffer_ptr,
                                     u32 buffer_count) {
  return BuildResourceLocator(0, u"xam", filename.GetValue(), buffer_ptr, buffer_count);
}

u32 XamGetSystemVersion_entry() {
  return 0;
}

void XCustomRegisterDynamicActions_entry() {}

u32 XGetAVPack_entry() {
  REXXAM_TRACE(
      "XGetAVPack -> "
      "6");
  return 6;
}

u32 GetGameRegion() {
  return 0xFFFFu;
}

u32 XGetGameRegion_entry() {
  return GetGameRegion();
}

u32 XGetLanguage_entry() {
  return static_cast<u32>(CurrentXAM().GetUserLanguage());
}

u32 XamGetCurrentTitleId_entry() {
  auto module = CurrentTitle().objects.Get<kernel::ILoadedModule>(
      CurrentTitle().modules.GetExecutable().value_or(0));
  if (module) {
    return module->GetTitleID();
  }
  return 0;
}

u32 XamGetExecutionId_entry(MappedU32 info_ptr) {
  *info_ptr = 0;
  auto module = CurrentTitle().objects.Get<kernel::ILoadedModule>(
      CurrentTitle().modules.GetExecutable().value_or(0));
  u32 guest_hdr_ptr = 0;
  if (!module ||
      kernel::XFAILED(
          module->GetOptHeaderGuestAddress(kernel::kXEXHeaderExecutionInfo, &guest_hdr_ptr)) ||
      guest_hdr_ptr == 0) {
    return kernel::X_STATUS_NOT_FOUND;
  }

  *info_ptr = guest_hdr_ptr;
  return kernel::X_STATUS_SUCCESS;
}

u32 XamLoaderSetLaunchData_entry(MappedVoid data, u32 size) {
  auto& loader_data = CurrentXAM().GetLoaderState();
  loader_data.launch_data_present = size ? true : false;
  loader_data.launch_data.resize(size);
  std::memcpy(loader_data.launch_data.data(), data, size);
  return 0;
}

u32 XamLoaderGetLaunchDataSize_entry(MappedU32 size_ptr) {
  if (!size_ptr) {
    return kernel::X_ERROR_INVALID_PARAMETER;
  }

  auto& loader_data = CurrentXAM().GetLoaderState();
  if (!loader_data.launch_data_present) {
    *size_ptr = 0;
    return kernel::X_ERROR_NOT_FOUND;
  }

  *size_ptr = static_cast<u32>(loader_data.launch_data.size());
  return kernel::X_ERROR_SUCCESS;
}

u32 XamLoaderGetLaunchData_entry(MappedVoid buffer_ptr, u32 buffer_size) {
  auto& loader_data = CurrentXAM().GetLoaderState();
  if (!loader_data.launch_data_present) {
    return kernel::X_ERROR_NOT_FOUND;
  }

  u32 copy_size = std::min(static_cast<u32>(loader_data.launch_data.size()), buffer_size);
  std::memcpy(buffer_ptr, loader_data.launch_data.data(), copy_size);
  return kernel::X_ERROR_SUCCESS;
}

void XamLoaderLaunchTitle_entry(MappedString raw_name_ptr, u32 flags) {
  {
    auto& loader_data = CurrentXAM().GetLoaderState();
    loader_data.launch_flags = flags;

    if (raw_name_ptr) {
      std::string path(raw_name_ptr.GetValue());
      if (path.empty()) {
        loader_data.launch_path = "game:\\default.xex";
      } else {
        if (rex::filesystem::utf8_find_name_from_guest_path(path) == path) {
          auto executable = CurrentTitle().objects.Get<kernel::ILoadedModule>(
              CurrentTitle().modules.GetExecutable().value_or(0));
          path = rex::filesystem::utf8_join_guest_paths(
              rex::filesystem::utf8_find_base_guest_path(executable->GetPath()), path);
        }
        loader_data.launch_path = path;
      }
    } else {
      REXXAM_INFO("The title exits to the dashboard");
    }
  }

  // This function does not return.
  CurrentTitle().exit.TerminateTitle();
}

void XamLoaderTerminateTitle_entry() {
  // This function does not return.
  CurrentTitle().exit.TerminateTitle();
}

namespace {

constexpr u32 kXamAllocWorkspace = 0x10000000;
constexpr u32 kXamAllocSystem = 0x04000000;

}  // namespace

u32 XamAlloc_entry(u32 flags, u32 size, MappedU32 out_ptr) {
  *out_ptr = 0;
  auto& xam = CurrentXAM();
  if (!(flags & (kXamAllocWorkspace | kXamAllocSystem))) {
    REXXAM_ERROR("XamAlloc: flags {:#010x} select no heap", flags);
    return kernel::X_ERROR_INVALID_PARAMETER;
  }
  if (flags & kXamAllocSystem) {
    const u32 address = xam.AllocHeap(size);
    if (!address) {
      REXXAM_WARN("XamAlloc: the XAM heap has no room for {:#x} bytes, flags {:#010x}", size,
                  flags);
      return kernel::X_ERROR_NOT_ENOUGH_MEMORY;
    }
    *out_ptr = address;
    return kernel::X_ERROR_SUCCESS;
  }
  Result<u32> address = CurrentTitle().workspace.Allocate(size);
  if (!address) {
    REXXAM_WARN("XamAlloc: the title workspace has no room for {:#x} bytes, flags {:#010x}", size,
                flags);
    return kernel::X_ERROR_NOT_ENOUGH_MEMORY;
  }
  *out_ptr = *address;
  return kernel::X_ERROR_SUCCESS;
}

u32 XamFree_entry(MappedU32 ptr) {
  if (CurrentXAM().FreeHeap(ptr.GetGuestAddress())) {
    return kernel::X_ERROR_SUCCESS;
  }
  CurrentTitle().workspace.Free(ptr.GetGuestAddress());
  return kernel::X_ERROR_SUCCESS;
}

u32 XamQueryLiveHiveW_entry([[maybe_unused]] MappedWString name,
                            [[maybe_unused]] MappedVoid out_buf, [[maybe_unused]] u32 out_size,
                            [[maybe_unused]] u32 type) {
  return kernel::X_STATUS_INVALID_PARAMETER_1;
}

u32 XamLoaderGetDvdTrayState_entry(MappedU32 out_state) {
  // 0 is tray open, 1 is tray closed with disc.
  if (out_state)
    *out_state = 1;
  return kernel::X_STATUS_SUCCESS;
}

u32 XamSwapDisc_entry(u32 disc_number) {
  REX_XAM_STUB("XamSwapDisc({})", static_cast<u32>(disc_number));
  return kernel::X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace rex

REX_XAM_BODY(__imp__XamFeatureEnabled, rex::xam::XamFeatureEnabled_entry)
REX_XAM_BODY(__imp__XamGetOnlineSchema, rex::xam::XamGetOnlineSchema_entry)
REX_XAM_BODY(__imp__XamFormatDateString, rex::xam::XamFormatDateString_entry)
REX_XAM_BODY(__imp__XamFormatTimeString, rex::xam::XamFormatTimeString_entry)
REX_XAM_BODY(__imp__XamBuildResourceLocator, rex::xam::XamBuildResourceLocator_entry)
REX_XAM_BODY(__imp__XamBuildGamercardResourceLocator,
             rex::xam::XamBuildGamercardResourceLocator_entry)
REX_XAM_BODY(__imp__XamBuildSharedSystemResourceLocator,
             rex::xam::XamBuildSharedSystemResourceLocator_entry)
REX_XAM_BODY(__imp__XamBuildLegacySystemResourceLocator,
             rex::xam::XamBuildLegacySystemResourceLocator_entry)
REX_XAM_BODY(__imp__XamBuildXamResourceLocator, rex::xam::XamBuildXamResourceLocator_entry)
REX_XAM_BODY(__imp__XamGetSystemVersion, rex::xam::XamGetSystemVersion_entry)
REX_XAM_BODY(__imp__XCustomRegisterDynamicActions, rex::xam::XCustomRegisterDynamicActions_entry)
REX_XAM_BODY(__imp__XGetAVPack, rex::xam::XGetAVPack_entry)
REX_XAM_BODY(__imp__XGetGameRegion, rex::xam::XGetGameRegion_entry)
REX_XAM_BODY(__imp__XGetLanguage, rex::xam::XGetLanguage_entry)
REX_XAM_BODY(__imp__XamGetCurrentTitleId, rex::xam::XamGetCurrentTitleId_entry)
REX_XAM_BODY(__imp__XamGetExecutionId, rex::xam::XamGetExecutionId_entry)
REX_XAM_BODY(__imp__XamLoaderSetLaunchData, rex::xam::XamLoaderSetLaunchData_entry)
REX_XAM_BODY(__imp__XamLoaderGetLaunchDataSize, rex::xam::XamLoaderGetLaunchDataSize_entry)
REX_XAM_BODY(__imp__XamLoaderGetLaunchData, rex::xam::XamLoaderGetLaunchData_entry)
REX_XAM_BODY(__imp__XamLoaderLaunchTitle, rex::xam::XamLoaderLaunchTitle_entry)
REX_XAM_BODY(__imp__XamLoaderTerminateTitle, rex::xam::XamLoaderTerminateTitle_entry)
REX_XAM_BODY(__imp__XamAlloc, rex::xam::XamAlloc_entry)
REX_XAM_BODY(__imp__XamFree, rex::xam::XamFree_entry)
REX_XAM_BODY(__imp__XamQueryLiveHiveW, rex::xam::XamQueryLiveHiveW_entry)
REX_XAM_BODY(__imp__XamLoaderGetDvdTrayState, rex::xam::XamLoaderGetDvdTrayState_entry)
REX_XAM_BODY(__imp__XamSwapDisc, rex::xam::XamSwapDisc_entry)
