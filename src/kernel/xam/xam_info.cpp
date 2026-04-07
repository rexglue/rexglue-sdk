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

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string/util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

#include <fmt/format.h>
#include <fmt/xchar.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

u32 XamFeatureEnabled_entry(u32 unk) {
  return 0;
}

// Empty stub schema binary.
uint8_t schema_bin[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
};

u32 XamGetOnlineSchema_entry() {
  static uint32_t schema_guest = 0;

  if (!schema_guest) {
    schema_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(8 + sizeof(schema_bin));
    auto schema = REX_KERNEL_MEMORY()->TranslateVirtual(schema_guest);
    std::memcpy(schema + 8, schema_bin, sizeof(schema_bin));
    memory::store_and_swap<uint32_t>(schema + 0, schema_guest + 8);
    memory::store_and_swap<uint32_t>(schema + 4, sizeof(schema_bin));
  }

  // return pointer to the schema ptr/schema size struct
  return schema_guest;
}

#if REX_PLATFORM_WIN32
static SYSTEMTIME xeGetLocalSystemTime(uint64_t filetime) {
  FILETIME t;
  t.dwHighDateTime = filetime >> 32;
  t.dwLowDateTime = (uint32_t)filetime;

  SYSTEMTIME st;
  SYSTEMTIME local_st;
  FileTimeToSystemTime(&t, &st);
  SystemTimeToTzSpecificLocalTime(NULL, &st, &local_st);
  return local_st;
}
#endif

void XamFormatDateString_entry(u32 unk, u64 filetime, mapped_void output_buffer, u32 output_count) {
  std::memset(output_buffer, 0, output_count * sizeof(char16_t));

// TODO: implement this for other platforms
#if REX_PLATFORM_WIN32
  auto st = xeGetLocalSystemTime(filetime);
  // TODO: format this depending on users locale?
  auto str = fmt::format(u"{:02d}/{:02d}/{}", st.wMonth, st.wDay, st.wYear);
  rex::string::util_copy_and_swap_truncating(output_buffer.as<char16_t*>(), str, output_count);
#else
  assert_always();
#endif
}

void XamFormatTimeString_entry(u32 unk, u64 filetime, mapped_void output_buffer, u32 output_count) {
  std::memset(output_buffer, 0, output_count * sizeof(char16_t));

// TODO: implement this for other platforms
#if REX_PLATFORM_WIN32
  auto st = xeGetLocalSystemTime(filetime);
  // TODO: format this depending on users locale?
  auto str = fmt::format(u"{:02d}:{:02d}", st.wHour, st.wMinute);
  rex::string::util_copy_and_swap_truncating(output_buffer.as<char16_t*>(), str, output_count);
#else
  assert_always();
#endif
}

u32 keXamBuildResourceLocator(uint64_t module, std::u16string_view container,
                              std::u16string_view resource, mapped_void buffer_ptr,
                              uint32_t buffer_count) {
  std::u16string path;
  if (!module) {
    path = fmt::format(u"file://media:/{}.xzp#{}", container, resource);
    REXKRNL_DEBUG("XamBuildResourceLocator({0}) returning locator to local file {0}.xzp",
                  rex::string::to_utf8(container));
  } else {
    path = fmt::format(u"section://{:X},{}#{}", (uint32_t)module, container, resource);
  }
  rex::string::util_copy_and_swap_truncating(buffer_ptr.as<char16_t*>(), path, buffer_count);
  return 0;
}

u32 XamBuildResourceLocator_entry(u64 module, mapped_wstring container, mapped_wstring resource,
                                  mapped_void buffer_ptr, u32 buffer_count) {
  return keXamBuildResourceLocator(module, container.value(), resource.value(), buffer_ptr,
                                   buffer_count);
}

u32 XamBuildGamercardResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                           u32 buffer_count) {
  // On an actual xbox these funcs would return a locator to xam.xex resources,
  // but for Xenia we can return a locator to the resources as local files. (big
  // thanks to MS for letting XamBuildResourceLocator return local file
  // locators!)

  // If you're running an app that'll need them, make sure to extract xam.xex
  // resources with xextool ("xextool -d . xam.xex") and add a .xzp extension.

  return keXamBuildResourceLocator(0, u"gamercrd", filename.value(), buffer_ptr, buffer_count);
}

u32 XamBuildSharedSystemResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                              u32 buffer_count) {
  // see notes inside XamBuildGamercardResourceLocator above
  return keXamBuildResourceLocator(0, u"shrdres", filename.value(), buffer_ptr, buffer_count);
}

u32 XamBuildLegacySystemResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                              u32 buffer_count) {
  return XamBuildSharedSystemResourceLocator_entry(filename, buffer_ptr, buffer_count);
}

u32 XamBuildXamResourceLocator_entry(mapped_wstring filename, mapped_void buffer_ptr,
                                     u32 buffer_count) {
  return keXamBuildResourceLocator(0, u"xam", filename.value(), buffer_ptr, buffer_count);
}

u32 XamGetSystemVersion_entry() {
  // eh, just picking one. If we go too low we may break new games, but
  // this value seems to be used for conditionally loading symbols and if
  // we pretend to be old we have less to worry with implementing.
  // 0x200A3200
  // 0x20096B00
  return 0;
}

void XCustomRegisterDynamicActions_entry() {
  // ???
}

u32 XGetAVPack_entry() {
  // DWORD
  // Not sure what the values are for this, but 6 is VGA.
  // Other likely values are 3/4/8 for HDMI or something.
  // Games seem to use this as a PAL check - if the result is not 3/4/6/8
  // they explode with errors if not in PAL mode.
  REXKRNL_IMPORT_RESULT("XGetAVPack", "6");
  return 6;
}

uint32_t xeXGetGameRegion() {
  return 0xFFFFu;
}

u32 XGetGameRegion_entry() {
  return xeXGetGameRegion();
}

u32 XGetLanguage_entry() {
  auto desired_language = XLanguage::kEnglish;

  // Switch the language based on game region.
  // TODO(benvanik): pull from xex header.
  uint32_t game_region = XEX_REGION_NTSCU;
  if (game_region & XEX_REGION_NTSCU) {
    desired_language = XLanguage::kEnglish;
  } else if (game_region & XEX_REGION_NTSCJ) {
    desired_language = XLanguage::kJapanese;
  }
  // Add more overrides?

  return uint32_t(desired_language);
}

u32 XamGetCurrentTitleId_entry() {
  // NOTE(tomc): Switched this up to get title ID from executable module instead of runtime
  // (emulator)
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  if (module) {
    return module->title_id();
  }
  return 0;
}

u32 XamGetExecutionId_entry(mapped_u32 info_ptr) {
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  assert_not_null(module);

  uint32_t guest_hdr_ptr;
  X_STATUS result = module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &guest_hdr_ptr);

  if (XFAILED(result)) {
    return result;
  }

  *info_ptr = guest_hdr_ptr;
  return X_STATUS_SUCCESS;
}

u32 XamLoaderSetLaunchData_entry(mapped_void data, u32 size) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  loader_data.launch_data_present = size ? true : false;
  loader_data.launch_data.resize(size);
  std::memcpy(loader_data.launch_data.data(), data, size);
  return 0;
}

u32 XamLoaderGetLaunchDataSize_entry(mapped_u32 size_ptr) {
  if (!size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (!loader_data.launch_data_present) {
    *size_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }

  *size_ptr = uint32_t(xam->loader_data().launch_data.size());
  return X_ERROR_SUCCESS;
}

u32 XamLoaderGetLaunchData_entry(mapped_void buffer_ptr, u32 buffer_size) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (!loader_data.launch_data_present) {
    return X_ERROR_NOT_FOUND;
  }

  uint32_t copy_size = std::min(uint32_t(loader_data.launch_data.size()), uint32_t(buffer_size));
  std::memcpy(buffer_ptr, loader_data.launch_data.data(), copy_size);
  return X_ERROR_SUCCESS;
}

void XamLoaderLaunchTitle_entry(mapped_string raw_name_ptr, u32 flags) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  auto& loader_data = xam->loader_data();
  loader_data.launch_flags = flags;

  // Translate the launch path to a full path.
  if (raw_name_ptr) {
    auto path = raw_name_ptr.value();
    if (path.empty()) {
      loader_data.launch_path = "game:\\default.xex";
    } else {
      if (rex::string::utf8_find_name_from_guest_path(path) == path) {
        path = rex::string::utf8_join_guest_paths(
            rex::string::utf8_find_base_guest_path(
                REX_KERNEL_STATE()->GetExecutableModule()->path()),
            path);
      }
      loader_data.launch_path = path;
    }
  } else {
    assert_always("Game requested exit to dashboard via XamLoaderLaunchTitle");
  }

  // This function does not return.
  REX_KERNEL_STATE()->TerminateTitle();
}

void XamLoaderTerminateTitle_entry() {
  // This function does not return.
  REX_KERNEL_STATE()->TerminateTitle();
}

u32 XamAlloc_entry(u32 unk, u32 size, mapped_u32 out_ptr) {
  assert_true(unk == 0);

  // Allocate from the heap. Not sure why XAM does this specially, perhaps
  // it keeps stuff in a separate heap?
  uint32_t ptr = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  *out_ptr = ptr;

  return X_ERROR_SUCCESS;
}

u32 XamFree_entry(mapped_u32 ptr) {
  REX_KERNEL_MEMORY()->SystemHeapFree(ptr.guest_address());

  return X_ERROR_SUCCESS;
}

u32 XamQueryLiveHiveW_entry(mapped_wstring name, mapped_void out_buf, u32 out_size,
                            u32 type /* guess */) {
  return X_STATUS_INVALID_PARAMETER_1;
}

u32 XamLoaderGetDvdTrayState_entry(mapped_u32 out_state) {
  // 0 = tray open, 1 = tray closed with disc
  if (out_state)
    *out_state = 1;
  return X_STATUS_SUCCESS;
}

u32 XamSwapDisc_entry(u32 disc_number) {
  // Stub for multi-disc games. Single-disc games (like Blue Dragon's reblue test)
  // don't need this, but the game may look it up dynamically via XexGetProcedureAddress.
  REXKRNL_DEBUG("XamSwapDisc({}) - stub, returning success", (uint32_t)disc_number);
  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamFeatureEnabled, rex::kernel::xam::XamFeatureEnabled_entry)
REX_EXPORT(__imp__XamGetOnlineSchema, rex::kernel::xam::XamGetOnlineSchema_entry)
REX_EXPORT(__imp__XamFormatDateString, rex::kernel::xam::XamFormatDateString_entry)
REX_EXPORT(__imp__XamFormatTimeString, rex::kernel::xam::XamFormatTimeString_entry)
REX_EXPORT(__imp__XamBuildResourceLocator, rex::kernel::xam::XamBuildResourceLocator_entry)
REX_EXPORT(__imp__XamBuildGamercardResourceLocator,
           rex::kernel::xam::XamBuildGamercardResourceLocator_entry)
REX_EXPORT(__imp__XamBuildSharedSystemResourceLocator,
           rex::kernel::xam::XamBuildSharedSystemResourceLocator_entry)
REX_EXPORT(__imp__XamBuildLegacySystemResourceLocator,
           rex::kernel::xam::XamBuildLegacySystemResourceLocator_entry)
REX_EXPORT(__imp__XamBuildXamResourceLocator, rex::kernel::xam::XamBuildXamResourceLocator_entry)
REX_EXPORT(__imp__XamGetSystemVersion, rex::kernel::xam::XamGetSystemVersion_entry)
REX_EXPORT(__imp__XCustomRegisterDynamicActions,
           rex::kernel::xam::XCustomRegisterDynamicActions_entry)
REX_EXPORT(__imp__XGetAVPack, rex::kernel::xam::XGetAVPack_entry)
REX_EXPORT(__imp__XGetGameRegion, rex::kernel::xam::XGetGameRegion_entry)
REX_EXPORT(__imp__XGetLanguage, rex::kernel::xam::XGetLanguage_entry)
REX_EXPORT(__imp__XamGetCurrentTitleId, rex::kernel::xam::XamGetCurrentTitleId_entry)
REX_EXPORT(__imp__XamGetExecutionId, rex::kernel::xam::XamGetExecutionId_entry)
REX_EXPORT(__imp__XamLoaderSetLaunchData, rex::kernel::xam::XamLoaderSetLaunchData_entry)
REX_EXPORT(__imp__XamLoaderGetLaunchDataSize, rex::kernel::xam::XamLoaderGetLaunchDataSize_entry)
REX_EXPORT(__imp__XamLoaderGetLaunchData, rex::kernel::xam::XamLoaderGetLaunchData_entry)
REX_EXPORT(__imp__XamLoaderLaunchTitle, rex::kernel::xam::XamLoaderLaunchTitle_entry)
REX_EXPORT(__imp__XamLoaderTerminateTitle, rex::kernel::xam::XamLoaderTerminateTitle_entry)
REX_EXPORT(__imp__XamAlloc, rex::kernel::xam::XamAlloc_entry)
REX_EXPORT(__imp__XamFree, rex::kernel::xam::XamFree_entry)
REX_EXPORT(__imp__XamQueryLiveHiveW, rex::kernel::xam::XamQueryLiveHiveW_entry)
REX_EXPORT(__imp__XamLoaderGetDvdTrayState, rex::kernel::xam::XamLoaderGetDvdTrayState_entry)
REX_EXPORT(__imp__XamSwapDisc, rex::kernel::xam::XamSwapDisc_entry)
