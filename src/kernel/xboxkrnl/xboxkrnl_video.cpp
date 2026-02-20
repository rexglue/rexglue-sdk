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

 // Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xboxkrnl/video.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/runtime/export_resolver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/kernel_state.h>
#include <rex/runtime/guest/function.h>
#include <rex/runtime/guest/types.h>
#include <rex/kernel/xboxkrnl/ordinals.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/kernel/xtypes.h>

REXCVAR_DEFINE_INT32(video_mode_width, 1280,
    "Guest video mode width in pixels",
    "GPU")
    .range(640, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(video_mode_height, 720,
    "Guest video mode height in pixels",
    "GPU")
    .range(480, 0x0FFF)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(resolution, "",
    "Common resolution preset for both guest video mode and startup window (for example: 720p, 1080p, 1440p, 4k, 1280x720)",
    "GPU")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(video_mode_refresh_rate, 60.0,
    "Guest video mode refresh rate in Hz",
    "GPU")
    .range(24.0, 240.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
// Display gamma type: 0 - linear, 1 - sRGB (CRT), 2 - BT.709 (HDTV), 3 - power
constexpr uint32_t kDisplayGammaType = 2;
// Display gamma power (used with gamma type 3)
constexpr double kDisplayGammaPower = 2.22222233;

bool TryGetInt32FlagValue(std::string_view flag_name, int32_t& value_out) {
  std::string flag_value = rex::cvar::GetFlagByName(flag_name);
  if (flag_value.empty()) {
    return false;
  }
  int32_t parsed_value = 0;
  auto [parse_end, parse_error] = std::from_chars(
      flag_value.data(), flag_value.data() + flag_value.size(), parsed_value);
  if (parse_error != std::errc() ||
      parse_end != flag_value.data() + flag_value.size()) {
    return false;
  }
  value_out = parsed_value;
  return true;
}

bool TryParsePositiveInt32(std::string_view value, int32_t& value_out) {
  if (value.empty()) {
    return false;
  }
  int32_t parsed_value = 0;
  auto [parse_end, parse_error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed_value);
  if (parse_error != std::errc() ||
      parse_end != value.data() + value.size() || parsed_value <= 0) {
    return false;
  }
  value_out = parsed_value;
  return true;
}

bool TryParseResolutionPreset(std::string_view resolution_value,
                              int32_t& width_out, int32_t& height_out) {
  std::string normalized;
  normalized.reserve(resolution_value.size());
  for (char c : resolution_value) {
    unsigned char c_unsigned = static_cast<unsigned char>(c);
    if (std::isspace(c_unsigned) || c == '_' || c == '-') {
      continue;
    }
    normalized.push_back(char(std::tolower(c_unsigned)));
  }
  if (normalized.empty()) {
    return false;
  }

  size_t x_position = normalized.find('x');
  if (x_position != std::string::npos && x_position > 0 &&
      (x_position + 1) < normalized.size()) {
    int32_t parsed_width = 0;
    int32_t parsed_height = 0;
    if (!TryParsePositiveInt32(std::string_view(normalized).substr(0, x_position),
                               parsed_width) ||
        !TryParsePositiveInt32(
            std::string_view(normalized).substr(x_position + 1),
            parsed_height)) {
      return false;
    }
    width_out = parsed_width;
    height_out = parsed_height;
    return true;
  }

  if (normalized == "480p") {
    width_out = 640;
    height_out = 480;
    return true;
  }
  if (normalized == "540p") {
    width_out = 960;
    height_out = 540;
    return true;
  }
  if (normalized == "720p") {
    width_out = 1280;
    height_out = 720;
    return true;
  }
  if (normalized == "900p") {
    width_out = 1600;
    height_out = 900;
    return true;
  }
  if (normalized == "1080p") {
    width_out = 1920;
    height_out = 1080;
    return true;
  }
  if (normalized == "1440p") {
    width_out = 2560;
    height_out = 1440;
    return true;
  }
  if (normalized == "1800p") {
    width_out = 3200;
    height_out = 1800;
    return true;
  }
  if (normalized == "2160p" || normalized == "4k") {
    width_out = 3840;
    height_out = 2160;
    return true;
  }
  return false;
}

bool TryGetResolutionPreset(int32_t& width_out, int32_t& height_out) {
  if (!rex::cvar::HasNonDefaultValue("resolution")) {
    return false;
  }
  std::string resolution_value = rex::cvar::GetFlagByName("resolution");
  if (resolution_value.empty()) {
    return false;
  }
  return TryParseResolutionPreset(resolution_value, width_out, height_out);
}

uint32_t GetConfiguredVideoModeWidth() {
  int32_t configured_width = REXCVAR_GET(video_mode_width);
  if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
    int32_t linked_width = 0;
    if (rex::cvar::HasNonDefaultValue("window_width") &&
        TryGetInt32FlagValue("window_width", linked_width) &&
        linked_width > 0) {
      configured_width = linked_width;
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (TryGetResolutionPreset(preset_width, preset_height)) {
        configured_width = preset_width;
      }
    }
  }
  return uint32_t(std::clamp(configured_width, 640, 0x0FFF));
}

uint32_t GetConfiguredVideoModeHeight() {
  int32_t configured_height = REXCVAR_GET(video_mode_height);
  if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
    int32_t linked_height = 0;
    if (rex::cvar::HasNonDefaultValue("window_height") &&
        TryGetInt32FlagValue("window_height", linked_height) &&
        linked_height > 0) {
      configured_height = linked_height;
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (TryGetResolutionPreset(preset_width, preset_height)) {
        configured_height = preset_height;
      }
    }
  }
  return uint32_t(std::clamp(configured_height, 480, 0x0FFF));
}

float GetConfiguredVideoModeRefreshRate() {
  double refresh_rate_hz =
      std::clamp(REXCVAR_GET(video_mode_refresh_rate), 24.0, 240.0);
  return float(refresh_rate_hz);
}
}  // namespace

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime::guest;

// https://web.archive.org/web/20150805074003/https://www.tweakoz.com/orkid/
// http://www.tweakoz.com/orkid/dox/d3/d52/xb360init_8cpp_source.html
// https://github.com/Free60Project/xenosfb/
// https://github.com/Free60Project/xenosfb/blob/master/src/xe.h
// https://github.com/gligli/libxemit
// https://web.archive.org/web/20090428095215/https://msdn.microsoft.com/en-us/library/bb313877.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313961.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313878.aspx
// https://web.archive.org/web/20090510235238/https://msdn.microsoft.com/en-us/library/bb313942.aspx
// https://svn.dd-wrt.com/browser/src/linux/universal/linux-3.8/drivers/gpu/drm/radeon/radeon_ring.c?rev=21595
// https://www.microsoft.com/en-za/download/details.aspx?id=5313 -- "Stripped
// Down Direct3D: Xbox 360 Command Buffer and Resource Management"

void VdGetCurrentDisplayGamma_entry(lpdword_t type_ptr, lpfloat_t power_ptr) {
  // 1 - sRGB.
  // 2 - TV (BT.709).
  // 3 - use the power written to *power_ptr.
  // Anything else - linear.
  // Used in D3D SetGammaRamp/SetPWLGamma to adjust the ramp for the display.
  *type_ptr = kDisplayGammaType;
  *power_ptr = float(kDisplayGammaPower);
}

struct X_D3DPRIVATE_RECT {
  rex::be<uint32_t> x1;  // 0x0
  rex::be<uint32_t> y1;  // 0x4
  rex::be<uint32_t> x2;  // 0x8
  rex::be<uint32_t> y2;  // 0xC
};
static_assert_size(X_D3DPRIVATE_RECT, 0x10);

struct X_D3DFILTER_PARAMETERS {
  rex::be<float> nyquist;         // 0x0
  rex::be<float> flicker_filter;  // 0x4
  rex::be<float> beta;            // 0x8
};
static_assert_size(X_D3DFILTER_PARAMETERS, 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;                 // 0x0
  rex::be<uint32_t> scaled_output_width;                 // 0x10
  rex::be<uint32_t> scaled_output_height;                // 0x14
  rex::be<uint32_t> vertical_filter_type;                // 0x18
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;    // 0x1C
  rex::be<uint32_t> horizontal_filter_type;              // 0x28
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;  // 0x2C
};
static_assert_size(X_D3DPRIVATE_SCALER_PARAMETERS, 0x38);

struct X_DISPLAY_INFO {
  rex::be<uint16_t> front_buffer_width;               // 0x0
  rex::be<uint16_t> front_buffer_height;              // 0x2
  uint8_t front_buffer_color_format;                 // 0x4
  uint8_t front_buffer_pixel_format;                 // 0x5
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;  // 0x8
  rex::be<uint16_t> display_window_overscan_left;     // 0x40
  rex::be<uint16_t> display_window_overscan_top;      // 0x42
  rex::be<uint16_t> display_window_overscan_right;    // 0x44
  rex::be<uint16_t> display_window_overscan_bottom;   // 0x46
  rex::be<uint16_t> display_width;                    // 0x48
  rex::be<uint16_t> display_height;                   // 0x4A
  rex::be<float> display_refresh_rate;                // 0x4C
  rex::be<uint32_t> display_interlaced;               // 0x50
  uint8_t display_color_format;                      // 0x54
  rex::be<uint16_t> actual_display_width;             // 0x56
};
static_assert_size(X_DISPLAY_INFO, 0x58);

void VdGetCurrentDisplayInformation_entry(
    pointer_t<X_DISPLAY_INFO> display_info) {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);
  display_info.Zero();
  display_info->front_buffer_width = (uint16_t)mode.display_width;
  display_info->front_buffer_height = (uint16_t)mode.display_height;

  display_info->scaler_parameters.scaler_source_rect.x2 = mode.display_width;
  display_info->scaler_parameters.scaler_source_rect.y2 = mode.display_height;
  display_info->scaler_parameters.scaled_output_width = mode.display_width;
  display_info->scaler_parameters.scaled_output_height = mode.display_height;
  display_info->scaler_parameters.horizontal_filter_type = 1;
  display_info->scaler_parameters.vertical_filter_type = 1;

  uint16_t overscan_x = uint16_t(uint32_t(mode.display_width) / 4);
  uint16_t overscan_y = uint16_t(uint32_t(mode.display_height) / 4);
  display_info->display_window_overscan_left = overscan_x;
  display_info->display_window_overscan_top = overscan_y;
  display_info->display_window_overscan_right = overscan_x;
  display_info->display_window_overscan_bottom = overscan_y;
  display_info->display_width = (uint16_t)mode.display_width;
  display_info->display_height = (uint16_t)mode.display_height;
  display_info->display_refresh_rate = mode.refresh_rate;
  display_info->actual_display_width = (uint16_t)mode.display_width;
}

void VdQueryVideoMode(X_VIDEO_MODE* video_mode) {
  // Exposed as CVARs so the guest can observe custom display settings.
  uint32_t display_width = GetConfiguredVideoModeWidth();
  uint32_t display_height = GetConfiguredVideoModeHeight();
  float refresh_rate_hz = GetConfiguredVideoModeRefreshRate();

  std::memset(video_mode, 0, sizeof(X_VIDEO_MODE));
  video_mode->display_width = display_width;
  video_mode->display_height = display_height;
  video_mode->is_interlaced = 0;
  video_mode->is_widescreen = display_width * 3 >= display_height * 4;
  video_mode->is_hi_def = display_width >= 1280 || display_height >= 720;
  video_mode->refresh_rate = refresh_rate_hz;
  video_mode->video_standard = 1;  // NTSC
  video_mode->unknown_0x8a = 0x4A;
  video_mode->unknown_0x01 = 0x01;
}

void VdQueryVideoMode_entry(pointer_t<X_VIDEO_MODE> video_mode) {
  VdQueryVideoMode(video_mode);
}

dword_result_t VdQueryVideoFlags_entry() {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);

  uint32_t flags = 0;
  flags |= mode.is_widescreen ? 1 : 0;
  flags |= mode.display_width >= 1024 ? 2 : 0;
  flags |= mode.display_width >= 1920 ? 4 : 0;

  return flags;
}

dword_result_t VdSetDisplayMode_entry(dword_t flags) {
  // Often 0x40000000.

  // 0?ccf000 00000000 00000000 000000r0

  // r: 0x00000002 |     1
  // f: 0x08000000 |    27
  // c: 0x30000000 | 28-29
  // ?: 0x40000000 |    30

  // r: 1 = Resolution is 720x480 or 720x576
  // f: 1 = Texture format is k_2_10_10_10 or k_2_10_10_10_AS_16_16_16_16
  // c: Color space (0 = RGB, 1 = ?, 2 = ?)
  // ?: (always set?)

  return 0;
}

dword_result_t VdSetDisplayModeOverride_entry(unknown_t unk0, unknown_t unk1,
                                              double_t refresh_rate,
                                              unknown_t unk3, unknown_t unk4) {
  // refresh_rate = 0, 50, 59.9, etc.
  return 0;
}

dword_result_t VdInitializeEngines_entry(unknown_t unk0, function_t callback,
                                         lpvoid_t arg, lpdword_t pfp_ptr,
                                         lpdword_t me_ptr) {
  // r3 = 0x4F810000
  // r4 = function ptr (cleanup callback?)
  // r5 = function arg
  // r6 = PFP Microcode
  // r7 = ME Microcode
  return 1;
}

void VdShutdownEngines_entry() {
  // Ignored for now.
  // Games seem to call an Initialize/Shutdown pair to query info, then
  // re-initialize.
}

dword_result_t VdGetGraphicsAsicID_entry() {
  // Games compare for < 0x10 and do VdInitializeEDRAM, else other
  // (retrain/etc).
  return 0x11;
}

dword_result_t VdEnableDisableClockGating_entry(dword_t enabled) {
  // Ignored, as it really doesn't matter.
  return 0;
}

void VdSetGraphicsInterruptCallback_entry(function_t callback,
                                          lpvoid_t user_data) {
  // callback takes 2 params
  // r3 = bool 0/1 - 0 is normal interrupt, 1 is some acquire/lock mumble
  // r4 = user_data (r4 of VdSetGraphicsInterruptCallback)
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->SetInterruptCallback(callback, user_data);
}

void VdInitializeRingBuffer_entry(lpvoid_t ptr, int_t size_log2) {
  // r3 = result of MmGetPhysicalAddress
  // r4 = log2(size)
  // Buffer pointers are from MmAllocatePhysicalMemory with WRITE_COMBINE.
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->InitializeRingBuffer(ptr, size_log2);
}

void VdEnableRingBufferRPtrWriteBack_entry(lpvoid_t ptr,
                                           int_t block_size_log2) {
  // r4 = log2(block size), 6, usually --- <=19
  auto graphics_system = kernel_state()->emulator()->graphics_system();
  graphics_system->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void VdGetSystemCommandBuffer_entry(lpunknown_t p0_ptr, lpunknown_t p1_ptr) {
  p0_ptr.Zero(0x94);
  memory::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
  memory::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
}

void VdSetSystemCommandBufferGpuIdentifierAddress_entry(lpunknown_t unk) {
  // r3 = 0x2B10(d3d?) + 8
}

// VdVerifyMEInitCommand
// r3
// r4 = 19
// no op?

dword_result_t VdInitializeScalerCommandBuffer_entry(
    dword_t scaler_source_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaler_source_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t scaled_output_xy,      // ((uint16_t)y << 16) | (uint16_t)x
    dword_t scaled_output_wh,      // ((uint16_t)h << 16) | (uint16_t)w
    dword_t front_buffer_wh,       // ((uint16_t)h << 16) | (uint16_t)w
    dword_t vertical_filter_type,  // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> vertical_filter_params,    //
    dword_t horizontal_filter_type,                              // 7?
    pointer_t<X_D3DFILTER_PARAMETERS> horizontal_filter_params,  //
    lpvoid_t unk9,                                               //
    lpvoid_t dest_ptr,  // Points to the first 80000000h where the memcpy
                        // sources from.
    dword_t dest_count  // Count in words.
) {
  // We could fake the commands here, but I'm not sure the game checks for
  // anything but success (non-zero ret).
  // For now, we just fill it with NOPs.
  auto dest = dest_ptr.as_array<uint32_t>();
  for (size_t i = 0; i < dest_count; ++i) {
    dest[i] = 0x80000000;
  }
  return (uint32_t)dest_count;
}

struct BufferScaling {
  rex::be<uint16_t> fb_width;
  rex::be<uint16_t> fb_height;
  rex::be<uint16_t> bb_width;
  rex::be<uint16_t> bb_height;
};
void AppendParam(string::StringBuffer* string_buffer, pointer_t<BufferScaling> param) {
  string_buffer->AppendFormat(
      "{:08X}(scale {}x{} -> {}x{}))", param.guest_address(),
      uint16_t(param->bb_width), uint16_t(param->bb_height),
      uint16_t(param->fb_width), uint16_t(param->fb_height));
}

dword_result_t VdCallGraphicsNotificationRoutines_entry(
    unknown_t unk0, pointer_t<BufferScaling> args_ptr) {

  assert_true(unk0 == 1);

  // TODO(benvanik): what does this mean, I forget:
  // callbacks get 0, r3, r4

  return 0;
}

dword_result_t VdIsHSIOTrainingSucceeded_entry() {
  // BOOL return value
  return 1;
}

dword_result_t VdPersistDisplay_entry(unknown_t unk0, lpdword_t unk1_ptr) {
  // unk1_ptr needs to be populated with a pointer passed to
  // MmFreePhysicalMemory(1, *unk1_ptr).
  if (unk1_ptr) {
    auto heap = kernel_memory()->LookupHeapByType(true, 16 * 1024);
    uint32_t unk1_value;
    heap->Alloc(64, 32, memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                memory::kMemoryProtectNoAccess, false, &unk1_value);
    *unk1_ptr = unk1_value;
  }

  return 1;
}

dword_result_t VdRetrainEDRAMWorker_entry(unknown_t unk0) { return 0; }

dword_result_t VdRetrainEDRAM_entry(unknown_t unk0, unknown_t unk1,
                                    unknown_t unk2, unknown_t unk3,
                                    unknown_t unk4, unknown_t unk5) {
  return 0;
}

void VdSwap_entry(
    lpvoid_t buffer_ptr,        // ptr into primary ringbuffer
    lpvoid_t fetch_ptr,         // frontbuffer Direct3D 9 texture header fetch
    lpunknown_t unk2,           // system writeback ptr
    lpunknown_t unk3,           // buffer from VdGetSystemCommandBuffer
    lpunknown_t unk4,           // from VdGetSystemCommandBuffer (0xBEEF0001)
    lpdword_t frontbuffer_ptr,  // ptr to frontbuffer address
    lpdword_t texture_format_ptr, lpdword_t color_space_ptr, lpdword_t width,
    lpdword_t height) {
  // All of these parameters are REQUIRED.
  assert(buffer_ptr);
  assert(fetch_ptr);
  assert(frontbuffer_ptr);
  assert(texture_format_ptr);
  assert(width);
  assert(height);

  namespace xenos = rex::graphics::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  memory::copy_and_swap_32_unaligned(
      &gpu_fetch, reinterpret_cast<uint32_t*>(fetch_ptr.host_address()), 6);

  // The fetch constant passed is not a true GPU fetch constant, but rather, the
  // fetch constant stored in the Direct3D 9 texture header, which contains the
  // address in one of the virtual mappings of the physical memory rather than
  // the physical address itself. We're emulating swapping in the GPU subsystem,
  // which works with GPU memory addresses (physical addresses directly) from
  // proper fetch constants like ones used to bind textures to shaders, not CPU
  // MMU addresses, so translation from virtual to physical is needed.
  uint32_t frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  assert_true(*frontbuffer_ptr == frontbuffer_virtual_address);
  uint32_t frontbuffer_physical_address =
      kernel_memory()->GetPhysicalAddress(frontbuffer_virtual_address);
  assert_true(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    // Xenia-specific safety check.
    REXKRNL_ERROR("VdSwap: Invalid front buffer virtual address 0x{:08X}",
           frontbuffer_virtual_address);
    return;
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;

  auto texture_format = rex::graphics::xenos::TextureFormat(texture_format_ptr.value());
  auto color_space = *color_space_ptr;
  assert_true(texture_format == rex::graphics::xenos::TextureFormat::k_8_8_8_8 ||
              texture_format ==
                  rex::graphics::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  assert_true(color_space == 0);  // RGB(0)
  assert_true(*width == 1 + gpu_fetch.size_2d.width);
  assert_true(*height == 1 + gpu_fetch.size_2d.height);

  // The caller seems to reserve 64 words (256b) in the primary ringbuffer
  // for this method to do what it needs. We just zero them out and send a
  // token value. It'd be nice to figure out what this is really doing so
  // that we could simulate it, though due to TCR I bet all games need to
  // use this method.
  buffer_ptr.Zero(64 * 4);

  uint32_t offset = 0;
  auto dwords = buffer_ptr.as_array<uint32_t>();

  // Write in the GPU texture fetch.
  dwords[offset++] =
      xenos::MakePacketType0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;

  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = rex::graphics::xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical_address;

  dwords[offset++] = *width;
  dwords[offset++] = *height;

  // Fill the rest of the buffer with NOP packets.
  for (uint32_t i = offset; i < 64; i++) {
    dwords[i] = xenos::MakePacketType2();
  }
}

void RegisterVideoExports(rex::runtime::ExportResolver* export_resolver,
                          KernelState* kernel_state) {
  auto memory = kernel_state->memory();

  // VdGlobalDevice (4b)
  // Pointer to a global D3D device. Games only seem to set this, so we don't
  // have to do anything. We may want to read it back later, though.
  uint32_t pVdGlobalDevice =
      memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGlobalDevice,
                                      pVdGlobalDevice);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalDevice), 0);

  // VdGlobalXamDevice (4b)
  // Pointer to the XAM D3D device, which we don't have.
  uint32_t pVdGlobalXamDevice =
      memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdGlobalXamDevice, pVdGlobalXamDevice);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalXamDevice), 0);

  // VdGpuClockInMHz (4b)
  // GPU clock. Xenos is 500MHz. Hope nothing is relying on this timing...
  uint32_t pVdGpuClockInMHz =
      memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", ordinals::VdGpuClockInMHz,
                                      pVdGpuClockInMHz);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGpuClockInMHz), 500);

  // VdHSIOCalibrationLock (28b)
  // CriticalSection.
  uint32_t pVdHSIOCalibrationLock =
      memory->SystemHeapAlloc(28, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping(
      "xboxkrnl.exe", ordinals::VdHSIOCalibrationLock, pVdHSIOCalibrationLock);
  auto hsio_lock =
      memory->TranslateVirtual<X_RTL_CRITICAL_SECTION*>(pVdHSIOCalibrationLock);
  xeRtlInitializeCriticalSectionAndSpinCount(hsio_lock, pVdHSIOCalibrationLock,
                                             10000);
}

}  // namespace rex::kernel::xboxkrnl

GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, rex::kernel::xboxkrnl::VdGetCurrentDisplayGamma_entry)
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, rex::kernel::xboxkrnl::VdGetCurrentDisplayInformation_entry)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, rex::kernel::xboxkrnl::VdQueryVideoMode_entry)
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, rex::kernel::xboxkrnl::VdQueryVideoFlags_entry)
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, rex::kernel::xboxkrnl::VdSetDisplayMode_entry)
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayModeOverride, rex::kernel::xboxkrnl::VdSetDisplayModeOverride_entry)
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, rex::kernel::xboxkrnl::VdInitializeEngines_entry)
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, rex::kernel::xboxkrnl::VdShutdownEngines_entry)
GUEST_FUNCTION_HOOK(__imp__VdGetGraphicsAsicID, rex::kernel::xboxkrnl::VdGetGraphicsAsicID_entry)
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, rex::kernel::xboxkrnl::VdEnableDisableClockGating_entry)
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, rex::kernel::xboxkrnl::VdSetGraphicsInterruptCallback_entry)
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, rex::kernel::xboxkrnl::VdInitializeRingBuffer_entry)
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, rex::kernel::xboxkrnl::VdEnableRingBufferRPtrWriteBack_entry)
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, rex::kernel::xboxkrnl::VdGetSystemCommandBuffer_entry)
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, rex::kernel::xboxkrnl::VdSetSystemCommandBufferGpuIdentifierAddress_entry)
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, rex::kernel::xboxkrnl::VdInitializeScalerCommandBuffer_entry)
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, rex::kernel::xboxkrnl::VdCallGraphicsNotificationRoutines_entry)
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, rex::kernel::xboxkrnl::VdIsHSIOTrainingSucceeded_entry)
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, rex::kernel::xboxkrnl::VdPersistDisplay_entry)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, rex::kernel::xboxkrnl::VdRetrainEDRAMWorker_entry)
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, rex::kernel::xboxkrnl::VdRetrainEDRAM_entry)
GUEST_FUNCTION_HOOK(__imp__VdSwap, rex::kernel::xboxkrnl::VdSwap_entry)
