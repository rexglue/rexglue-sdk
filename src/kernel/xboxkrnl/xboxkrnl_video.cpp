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

#include <algorithm>
#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/gpu.h>
#include <rex/xenos/texture_info.h>
#include <rex/xenos/register_file.h>
#include <rex/xenos/xenos.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging/log.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/string/buffer.h>
#include <rex/runtime/module_object.h>
#include <rex/runtime/xtypes.h>

namespace {
/** Display gamma type, 0 linear, 1 sRGB (CRT), 2 BT.709 (HDTV) or 3 power. */
constexpr u32 kDisplayGammaType = 2;
/** Display gamma power, used with gamma type 3. */
constexpr f64 kDisplayGammaPower = 2.22222233;

void WarnNoGPUEmulation(const char* export_name, std::atomic<bool>& warned) {
  if (!warned.exchange(true)) {
    REXLOG_WARN(kernel, "{}: tool mode runs without a GPU, call ignored", export_name);
  }
}
}  // namespace

namespace rex::kernel::xboxkrnl {
using namespace rex::runtime;

REX_EXPORT_STUB(__imp__VdBlockUntilGUIIdle);
REX_EXPORT_STUB(__imp__VdDisplayFatalError);
REX_EXPORT_STUB(__imp__VdEnableClosedCaption);
REX_EXPORT_STUB(__imp__VdEnableDisablePowerSavingMode);
REX_EXPORT_STUB(__imp__VdGenerateGPUCSCCoefficients);
REX_EXPORT_STUB(__imp__VdGetClosedCaptionReadyStatus);
REX_EXPORT_STUB(__imp__VdGetDisplayModeOverride);
REX_EXPORT_STUB(__imp__VdInitializeScaler);
REX_EXPORT_STUB(__imp__VdQuerySystemCommandBuffer);
REX_EXPORT_STUB(__imp__VdReadDVERegisterUlong);
REX_EXPORT_STUB(__imp__VdReadWriteHSIOCalibrationFlag);
REX_EXPORT_STUB(__imp__VdRegisterGraphicsNotification);
REX_EXPORT_STUB(__imp__VdRegisterXamGraphicsNotification);
REX_EXPORT_STUB(__imp__VdSendClosedCaptionData);
REX_EXPORT_STUB(__imp__VdSetCGMSOption);
REX_EXPORT_STUB(__imp__VdSetColorProfileAdjustment);
REX_EXPORT_STUB(__imp__VdSetCscMatricesOverride);
REX_EXPORT_STUB(__imp__VdSetHDCPOption);
REX_EXPORT_STUB(__imp__VdSetMacrovisionOption);
REX_EXPORT_STUB(__imp__VdSetSystemCommandBuffer);
REX_EXPORT_STUB(__imp__VdSetWSSData);
REX_EXPORT_STUB(__imp__VdSetWSSOption);
REX_EXPORT_STUB(__imp__VdTurnDisplayOff);
REX_EXPORT_STUB(__imp__VdTurnDisplayOn);
REX_EXPORT_STUB(__imp__VdWriteDVERegisterUlong);
REX_EXPORT_STUB(__imp__VdInitializeEDRAM);
REX_EXPORT_STUB(__imp__VdReadEEDIDBlock);
REX_EXPORT_STUB(__imp__VdEnumerateVideoModes);
REX_EXPORT_STUB(__imp__VdEnableHDCP);
REX_EXPORT_STUB(__imp__VdRegisterHDCPNotification);
REX_EXPORT_STUB(__imp__VdGetDisplayDiscoveryData);
REX_EXPORT_STUB(__imp__VdStartDisplayDiscovery);
REX_EXPORT_STUB(__imp__VdSetHDCPRevocationList);
REX_EXPORT_STUB(__imp__VdEnableWMAProOverHDMI);
REX_EXPORT_STUB(__imp__VdQueryRealVideoMode);
REX_EXPORT_STUB(__imp__VdSetCGMSState);
REX_EXPORT_STUB(__imp__VdSetSCMSState);
REX_EXPORT_STUB(__imp__VdGetOption);
REX_EXPORT_STUB(__imp__VdSetOption);
REX_EXPORT_STUB(__imp__VdQueryVideoCapabilities);
REX_EXPORT_STUB(__imp__VdGet3dVideoFormat);
REX_EXPORT_STUB(__imp__VdGetWSS2Data);
REX_EXPORT_STUB(__imp__VdSet3dVideoFormat);
REX_EXPORT_STUB(__imp__VdSetWSS2Data);
REX_EXPORT_STUB(__imp__VdSetStudioRGBMode);

/**
 * Returns the display gamma.
 *
 * The gamma type is 1 for sRGB, 2 for TV (BT.709), 3 to use the power in power_ptr, otherwise
 * linear.
 *
 * @param type_ptr Receives the gamma type.
 * @param power_ptr Receives the gamma power.
 */
void VdGetCurrentDisplayGamma_entry(MappedU32 type_ptr, MappedF32 power_ptr) {
  *type_ptr = kDisplayGammaType;
  *power_ptr = static_cast<f32>(kDisplayGammaPower);
}

struct X_D3DPRIVATE_RECT {
  be_u32 x1;
  be_u32 y1;
  be_u32 x2;
  be_u32 y2;
};
REX_STATIC_ASSERT_SIZE(X_D3DPRIVATE_RECT, 0x10);

struct X_D3DFILTER_PARAMETERS {
  be_f32 nyquist;
  be_f32 flicker_filter;
  be_f32 beta;
};
REX_STATIC_ASSERT_SIZE(X_D3DFILTER_PARAMETERS, 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;
  be_u32 scaled_output_width;
  be_u32 scaled_output_height;
  be_u32 vertical_filter_type;
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;
  be_u32 horizontal_filter_type;
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;
};
REX_STATIC_ASSERT_SIZE(X_D3DPRIVATE_SCALER_PARAMETERS, 0x38);

struct X_DISPLAY_INFO {
  be_u16 front_buffer_width;
  be_u16 front_buffer_height;
  u8 front_buffer_color_format;
  u8 front_buffer_pixel_format;
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;
  be_u16 display_window_overscan_left;
  be_u16 display_window_overscan_top;
  be_u16 display_window_overscan_right;
  be_u16 display_window_overscan_bottom;
  be_u16 display_width;
  be_u16 display_height;
  be_f32 display_refresh_rate;
  be_u32 display_interlaced;
  u8 display_color_format;
  be_u16 actual_display_width;
};
REX_STATIC_ASSERT_SIZE(X_DISPLAY_INFO, 0x58);

void VdGetCurrentDisplayInformation_entry(GuestPtrT<X_DISPLAY_INFO> display_info) {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);
  display_info.Zero();
  display_info->front_buffer_width = static_cast<u16>(mode.display_width);
  display_info->front_buffer_height = static_cast<u16>(mode.display_height);

  display_info->scaler_parameters.scaler_source_rect.x2 = mode.display_width;
  display_info->scaler_parameters.scaler_source_rect.y2 = mode.display_height;
  display_info->scaler_parameters.scaled_output_width = mode.display_width;
  display_info->scaler_parameters.scaled_output_height = mode.display_height;
  display_info->scaler_parameters.horizontal_filter_type = 1;
  display_info->scaler_parameters.vertical_filter_type = 1;

  u16 overscan_x = static_cast<u16>(static_cast<u32>(mode.display_width) / 4);
  u16 overscan_y = static_cast<u16>(static_cast<u32>(mode.display_height) / 4);
  display_info->display_window_overscan_left = overscan_x;
  display_info->display_window_overscan_top = overscan_y;
  display_info->display_window_overscan_right = overscan_x;
  display_info->display_window_overscan_bottom = overscan_y;
  display_info->display_width = static_cast<u16>(mode.display_width);
  display_info->display_height = static_cast<u16>(mode.display_height);
  display_info->display_refresh_rate = mode.refresh_rate;
  display_info->actual_display_width = static_cast<u16>(mode.display_width);
}

void VdQueryVideoMode(X_VIDEO_MODE* video_mode) {
  const runtime::VideoMode& mode = runtime::GetCurrent().GetVideoMode();
  const u32 display_width = mode.width;
  const u32 display_height = mode.height;
  const f32 refresh_rate_hz = mode.refresh_rate;

  std::memset(video_mode, 0, sizeof(X_VIDEO_MODE));
  video_mode->display_width = display_width;
  video_mode->display_height = display_height;
  video_mode->is_interlaced = 0;
  video_mode->is_widescreen = display_width * 3 >= display_height * 4;
  video_mode->is_hi_def = display_width >= 1280 || display_height >= 720;
  video_mode->refresh_rate = refresh_rate_hz;
  // video_standard 1 is NTSC.
  video_mode->video_standard = 1;
  video_mode->unknown_0x8a = 0x4A;
  video_mode->unknown_0x01 = 0x01;
}

void VdQueryVideoMode_entry(GuestPtrT<X_VIDEO_MODE> video_mode) {
  VdQueryVideoMode(video_mode);
}

u32 VdQueryVideoFlags_entry() {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);

  u32 flags = 0;
  flags |= mode.is_widescreen ? 1 : 0;
  flags |= mode.display_width >= 1024 ? 2 : 0;
  flags |= mode.display_width >= 1920 ? 4 : 0;

  return flags;
}

/**
 * Sets the display mode. Ignored.
 *
 * Bit 1 of the flags is set when the resolution is 720x480 or 720x576. Bit 27 is set when the
 * texture format is k_2_10_10_10 or k_2_10_10_10_AS_16_16_16_16. Bits 28 to 29 select the color
 * space, 0 for RGB.
 *
 * @param flags The mode flags.
 * @return 0.
 */
u32 VdSetDisplayMode_entry([[maybe_unused]] u32 flags) {
  return 0;
}

/**
 * Overrides the display mode. Ignored.
 *
 * @param unk0 Unused.
 * @param unk1 Unused.
 * @param refresh_rate The refresh rate, 0, 50 or 59.9.
 * @param unk3 Unused.
 * @param unk4 Unused.
 * @return 0.
 */
u32 VdSetDisplayModeOverride_entry([[maybe_unused]] u32 unk0, [[maybe_unused]] u32 unk1,
                                   [[maybe_unused]] f64 refresh_rate, [[maybe_unused]] u32 unk3,
                                   [[maybe_unused]] u32 unk4) {
  return 0;
}

/**
 * Initializes the graphics engines. Ignored.
 *
 * @param unk0 Always 0x4F810000.
 * @param callback The cleanup function pointer.
 * @param arg The argument of callback.
 * @param pfp_ptr The PFP microcode.
 * @param me_ptr The ME microcode.
 * @return 1.
 */
u32 VdInitializeEngines_entry([[maybe_unused]] u32 unk0, [[maybe_unused]] u32 callback,
                              [[maybe_unused]] MappedVoid arg, [[maybe_unused]] MappedU32 pfp_ptr,
                              [[maybe_unused]] MappedU32 me_ptr) {
  return 1;
}

/** Shuts down the graphics engines. Ignored. */
void VdShutdownEngines_entry() {}

/**
 * Returns the graphics ASIC id.
 *
 * @return 0x11.
 */
u32 VdGetGraphicsAsicID_entry() {
  return 0x11;
}

/**
 * Enables or disables clock gating. Ignored.
 *
 * @param enabled Unused.
 * @return 0.
 */
u32 VdEnableDisableClockGating_entry([[maybe_unused]] u32 enabled) {
  return 0;
}

/**
 * Sets the graphics interrupt callback.
 *
 * The callback is called with r3 set to 0 for a normal interrupt, otherwise 1, and r4 set to
 * user_data.
 *
 * @param callback The interrupt routine.
 * @param user_data The context passed to callback.
 */
void VdSetGraphicsInterruptCallback_entry(u32 callback, MappedVoid user_data) {
  auto* gpu = runtime::GetCurrent().GetGPU();
  if (!gpu) {
    static std::atomic<bool> warned{false};
    WarnNoGPUEmulation("VdSetGraphicsInterruptCallback", warned);
    return;
  }
  gpu->SetInterruptCallback(callback, user_data.GetGuestAddress());
}

/**
 * Initializes the primary ring buffer.
 *
 * The buffer is memory from MmAllocatePhysicalMemory with kXPageWriteCombine.
 *
 * @param ptr The physical address of the buffer from MmGetPhysicalAddress.
 * @param size_log2 The log2 of the size.
 */
void VdInitializeRingBuffer_entry(MappedVoid ptr, i32 size_log2) {
  auto* gpu = runtime::GetCurrent().GetGPU();
  if (!gpu) {
    static std::atomic<bool> warned{false};
    WarnNoGPUEmulation("VdInitializeRingBuffer", warned);
    return;
  }
  gpu->InitializeRingBuffer(ptr.GetGuestAddress(), size_log2);
}

/**
 * Enables write back of the ring buffer read pointer.
 *
 * @param ptr The write back address.
 * @param block_size_log2 The log2 of the block size, usually 6 and at most 19.
 */
void VdEnableRingBufferRPtrWriteBack_entry(MappedVoid ptr, i32 block_size_log2) {
  auto* gpu = runtime::GetCurrent().GetGPU();
  if (!gpu) {
    static std::atomic<bool> warned{false};
    WarnNoGPUEmulation("VdEnableRingBufferRPtrWriteBack", warned);
    return;
  }
  gpu->EnableReadPointerWriteBack(ptr.GetGuestAddress(), block_size_log2);
}

void VdGetSystemCommandBuffer_entry(MappedVoid p0_ptr, MappedVoid p1_ptr) {
  p0_ptr.Zero(0x94);
  memory::store<be_u32>(p0_ptr, 0xBEEF0000);
  memory::store<be_u32>(p1_ptr, 0xBEEF0001);
}

void VdSetSystemCommandBufferGpuIdentifierAddress_entry([[maybe_unused]] MappedVoid unk) {}

/**
 * Initializes the scaler command buffer by filling it with NOP packets (0x80000000).
 *
 * @param scaler_source_xy The source position, packed as (y << 16) | x with 16 bit fields.
 * @param scaler_source_wh The source size, packed as (h << 16) | w with 16 bit fields.
 * @param scaled_output_xy The output position, packed as (y << 16) | x with 16 bit fields.
 * @param scaled_output_wh The output size, packed as (h << 16) | w with 16 bit fields.
 * @param front_buffer_wh The front buffer size, packed as (h << 16) | w with 16 bit fields.
 * @param vertical_filter_type Unused.
 * @param vertical_filter_params Unused.
 * @param horizontal_filter_type Unused.
 * @param horizontal_filter_params Unused.
 * @param unk9 Unused.
 * @param dest_ptr The command buffer.
 * @param dest_count The size of dest_ptr in words.
 * @return The count of words written.
 */
u32 VdInitializeScalerCommandBuffer_entry(
    [[maybe_unused]] u32 scaler_source_xy, [[maybe_unused]] u32 scaler_source_wh,
    [[maybe_unused]] u32 scaled_output_xy, [[maybe_unused]] u32 scaled_output_wh,
    [[maybe_unused]] u32 front_buffer_wh, [[maybe_unused]] u32 vertical_filter_type,
    [[maybe_unused]] GuestPtrT<X_D3DFILTER_PARAMETERS> vertical_filter_params,
    [[maybe_unused]] u32 horizontal_filter_type,
    [[maybe_unused]] GuestPtrT<X_D3DFILTER_PARAMETERS> horizontal_filter_params,
    [[maybe_unused]] MappedVoid unk9, MappedVoid dest_ptr, u32 dest_count) {
  auto dest = dest_ptr.AsArray<u32>();
  for (size_t i = 0; i < dest_count; ++i) {
    dest[i] = 0x80000000;
  }
  return dest_count;
}

struct BufferScaling {
  be_u16 fb_width;
  be_u16 fb_height;
  be_u16 bb_width;
  be_u16 bb_height;
};

u32 VdCallGraphicsNotificationRoutines_entry(u32 unk0,
                                             [[maybe_unused]] GuestPtrT<BufferScaling> args_ptr) {
  REX_ASSERT(unk0 == 1);

  return 0;
}

/**
 * Reports whether HSIO training succeeded.
 *
 * @return 1 (BOOL TRUE).
 */
u32 VdIsHSIOTrainingSucceeded_entry() {
  return 1;
}

/**
 * Persists the display.
 *
 * @param unk0 Unused.
 * @param unk1_ptr Receives a pointer that the guest passes to MmFreePhysicalMemory(1, *unk1_ptr).
 * @return 1.
 */
u32 VdPersistDisplay_entry([[maybe_unused]] u32 unk0, MappedU32 unk1_ptr) {
  if (unk1_ptr) {
    auto heap = runtime::GetCurrent().GetMemory()->LookupHeapByType(true, 16 * 1024);
    u32 unk1_value;
    heap->Alloc(64, 32, runtime::kMemoryAllocationReserve | runtime::kMemoryAllocationCommit,
                runtime::kMemoryProtectNoAccess, false, &unk1_value);
    *unk1_ptr = unk1_value;
  }

  return 1;
}

u32 VdRetrainEDRAMWorker_entry([[maybe_unused]] u32 unk0) {
  return 0;
}

u32 VdRetrainEDRAM_entry([[maybe_unused]] u32 unk0, [[maybe_unused]] u32 unk1,
                         [[maybe_unused]] u32 unk2, [[maybe_unused]] u32 unk3,
                         [[maybe_unused]] u32 unk4, [[maybe_unused]] u32 unk5) {
  return 0;
}

/**
 * Swaps the front buffer by writing the swap packets into the primary ring buffer. The caller
 * reserves 64 words (256 bytes) in the ring buffer, which are zeroed, filled with the fetch
 * constant write and padded with NOP packets.
 *
 * The fetch constant is the one stored in the Direct3D 9 texture header, which holds an address in
 * a virtual mapping of the physical memory. The GPU subsystem works with physical addresses, so
 * the address is translated.
 *
 * All parameters except the unused ones are required.
 *
 * @param buffer_ptr The pointer into the primary ring buffer.
 * @param fetch_ptr The front buffer Direct3D 9 texture header fetch.
 * @param unk2 The system writeback pointer.
 * @param unk3 The buffer from VdGetSystemCommandBuffer.
 * @param unk4 The value from VdGetSystemCommandBuffer (0xBEEF0001).
 * @param frontbuffer_ptr The pointer to the front buffer address.
 * @param texture_format_ptr The texture format, k_8_8_8_8 or k_2_10_10_10_AS_16_16_16_16.
 * @param color_space_ptr The color space, 0 for RGB.
 * @param width The front buffer width.
 * @param height The front buffer height.
 */
void VdSwap_entry(MappedVoid buffer_ptr, MappedVoid fetch_ptr, [[maybe_unused]] MappedVoid unk2,
                  [[maybe_unused]] MappedVoid unk3, [[maybe_unused]] MappedVoid unk4,
                  MappedU32 frontbuffer_ptr, MappedU32 texture_format_ptr,
                  MappedU32 color_space_ptr, MappedU32 width, MappedU32 height) {
  assert(buffer_ptr);
  assert(fetch_ptr);
  assert(frontbuffer_ptr);
  assert(texture_format_ptr);
  assert(width);
  assert(height);

  xenos::TextureFetch gpu_fetch;
  memory::copy_and_swap_32_unaligned(&gpu_fetch, reinterpret_cast<u32*>(fetch_ptr.GetHostAddress()),
                                     6);

  u32 frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  REX_ASSERT(*frontbuffer_ptr == frontbuffer_virtual_address);
  u32 frontbuffer_physical_address =
      runtime::GetCurrent().GetMemory()->GetPhysicalAddress(frontbuffer_virtual_address);
  REX_ASSERT(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    REXLOG_ERROR(kernel, "VdSwap: Invalid front buffer virtual address 0x{:08X}",
                 frontbuffer_virtual_address);
    return;
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;

  auto texture_format = rex::xenos::TextureFormat(texture_format_ptr.GetValue());
  auto color_space = *color_space_ptr;
  REX_ASSERT(texture_format == rex::xenos::TextureFormat::k_8_8_8_8 ||
             texture_format == rex::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  REX_ASSERT(color_space == 0);
  REX_ASSERT(*width == 1 + gpu_fetch.size_2d.width);
  REX_ASSERT(*height == 1 + gpu_fetch.size_2d.height);

  buffer_ptr.Zero(size_t{64} * 4);

  u32 offset = 0;
  auto dwords = buffer_ptr.AsArray<u32>();

  dwords[offset++] = xenos::BuildPacketType0(
      std::to_underlying(rex::xenos::Register::SHADER_CONSTANT_FETCH_00_0), 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;

  dwords[offset++] = xenos::BuildPacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = rex::xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical_address;

  dwords[offset++] = *width;
  dwords[offset++] = *height;

  for (u32 i = offset; i < 64; i++) {
    dwords[i] = xenos::BuildPacketType2();
  }
}

void RegisterVideoExports(ModuleObject& module, Runtime& runtime) {
  auto memory = runtime.GetMemory();

  // Titles only write the global D3D device pointer.
  u32 p_vd_global_device = memory->SystemHeapAlloc(4, 32, runtime::kSystemHeapPhysical);
  module.SetVariable(0x01BE, p_vd_global_device);
  *memory->TranslateVirtual<be_u32*>(p_vd_global_device) = 0;

  // No XAM D3D device exists, so the pointer stays 0.
  u32 p_vd_global_xam_device = memory->SystemHeapAlloc(4, 32, runtime::kSystemHeapPhysical);
  module.SetVariable(0x01BF, p_vd_global_xam_device);
  *memory->TranslateVirtual<be_u32*>(p_vd_global_xam_device) = 0;

  // The Xenos clock is 500 MHz.
  u32 p_vd_gpu_clock_in_mhz = memory->SystemHeapAlloc(4, 32, runtime::kSystemHeapPhysical);
  module.SetVariable(0x01C0, p_vd_gpu_clock_in_mhz);
  *memory->TranslateVirtual<be_u32*>(p_vd_gpu_clock_in_mhz) = 500;

  u32 p_vd_hsio_calibration_lock = memory->SystemHeapAlloc(28, 32, runtime::kSystemHeapPhysical);
  module.SetVariable(0x01C1, p_vd_hsio_calibration_lock);
  auto hsio_lock = memory->TranslateVirtual<X_RTL_CRITICAL_SECTION*>(p_vd_hsio_calibration_lock);
  RtlInitializeCriticalSectionAndSpinCount(hsio_lock, 10000);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__VdGetCurrentDisplayGamma, rex::kernel::xboxkrnl::VdGetCurrentDisplayGamma_entry)
REX_EXPORT(__imp__VdGetCurrentDisplayInformation,
           rex::kernel::xboxkrnl::VdGetCurrentDisplayInformation_entry)
REX_EXPORT(__imp__VdQueryVideoMode, rex::kernel::xboxkrnl::VdQueryVideoMode_entry)
REX_EXPORT(__imp__VdQueryVideoFlags, rex::kernel::xboxkrnl::VdQueryVideoFlags_entry)
REX_EXPORT(__imp__VdSetDisplayMode, rex::kernel::xboxkrnl::VdSetDisplayMode_entry)
REX_EXPORT(__imp__VdSetDisplayModeOverride, rex::kernel::xboxkrnl::VdSetDisplayModeOverride_entry)
REX_EXPORT(__imp__VdInitializeEngines, rex::kernel::xboxkrnl::VdInitializeEngines_entry)
REX_EXPORT(__imp__VdShutdownEngines, rex::kernel::xboxkrnl::VdShutdownEngines_entry)
REX_EXPORT(__imp__VdGetGraphicsAsicID, rex::kernel::xboxkrnl::VdGetGraphicsAsicID_entry)
REX_EXPORT(__imp__VdEnableDisableClockGating,
           rex::kernel::xboxkrnl::VdEnableDisableClockGating_entry)
REX_EXPORT(__imp__VdSetGraphicsInterruptCallback,
           rex::kernel::xboxkrnl::VdSetGraphicsInterruptCallback_entry)
REX_EXPORT(__imp__VdInitializeRingBuffer, rex::kernel::xboxkrnl::VdInitializeRingBuffer_entry)
REX_EXPORT(__imp__VdEnableRingBufferRPtrWriteBack,
           rex::kernel::xboxkrnl::VdEnableRingBufferRPtrWriteBack_entry)
REX_EXPORT(__imp__VdGetSystemCommandBuffer, rex::kernel::xboxkrnl::VdGetSystemCommandBuffer_entry)
REX_EXPORT(__imp__VdSetSystemCommandBufferGpuIdentifierAddress,
           rex::kernel::xboxkrnl::VdSetSystemCommandBufferGpuIdentifierAddress_entry)
REX_EXPORT(__imp__VdInitializeScalerCommandBuffer,
           rex::kernel::xboxkrnl::VdInitializeScalerCommandBuffer_entry)
REX_EXPORT(__imp__VdCallGraphicsNotificationRoutines,
           rex::kernel::xboxkrnl::VdCallGraphicsNotificationRoutines_entry)
REX_EXPORT(__imp__VdIsHSIOTrainingSucceeded, rex::kernel::xboxkrnl::VdIsHSIOTrainingSucceeded_entry)
REX_EXPORT(__imp__VdPersistDisplay, rex::kernel::xboxkrnl::VdPersistDisplay_entry)
REX_EXPORT(__imp__VdRetrainEDRAMWorker, rex::kernel::xboxkrnl::VdRetrainEDRAMWorker_entry)
REX_EXPORT(__imp__VdRetrainEDRAM, rex::kernel::xboxkrnl::VdRetrainEDRAM_entry)
REX_EXPORT(__imp__VdSwap, rex::kernel::xboxkrnl::VdSwap_entry)
