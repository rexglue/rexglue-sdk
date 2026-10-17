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
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include "gpu/xenos/d3d12/command_processor.h"
#include "gpu/xenos/d3d12/shared_memory.h"
#include "gpu/xenos/d3d12/texture_cache.h"
#include <rex/xenos/texture_info.h>
#include <rex/xenos/texture_layout.h>
#include <rex/xenos/xenos.h>
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include "gpu/xenos/host/d3d12/d3d12_upload_buffer_pool.h"
#include "gpu/xenos/host/d3d12/d3d12_objects.h"

#include "gpu/xenos/cvar_table_pair.h"

namespace rex::gpu::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_128bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_16bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_32bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_64bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_8bpb_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_bgrg8_rgbg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_ctx1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_float_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxn_rg8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3a_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt3aas1111_bgra4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_dxt5a_r8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_grgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r10g11b11_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r11g11b10_rgba16_snorm_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r4g4b4a4_b4g4r4a4_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b5a1_b5g5r5a1_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g5b6_b5g6r5_swizzle_rbga_scaled_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_cs.h"
#include "../shaders/bytecode/d3d12_5_1/texture_load_r5g6b5_b5g6r5_scaled_cs.h"
}  // namespace shaders

const D3D12TextureCache::HostFormat D3D12TextureCache::kHostFormats[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_1_5_5_5
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_5_6_5
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBB},
    // k_6_5_5, host green bits in blue and blue bits in green.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, REXGPU_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kLoadShaderIndexGBGR8ToGRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexGBGR8ToRGB8, xenos::REXGPU_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, kLoadShaderIndexBGRG8ToRGBG8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexBGRG8ToRGB8, xenos::REXGPU_SWIZZLE_RGBB},
    // k_16_16_EDRAM, not usable as a texture with its -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_4_4_4_4
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM, not usable as a texture with its -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_24_8, R32_FLOAT for depth since sampling stencil would need another SRV.
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexDXNToRG8,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBB},
    // k_DXT3A, R8_UNORM matches the BC2 size without its 4x4 alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8,
     xenos::REXGPU_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexDXT3AAs1111ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::REXGPU_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::REXGPU_SWIZZLE_RGBA},
};

D3D12TextureCache::D3D12TextureCache(Key, const xenos::RegisterFile& register_file,
                                     D3D12SharedMemory& shared_memory, u32 draw_resolution_scale_x,
                                     u32 draw_resolution_scale_y,
                                     D3D12CommandProcessor& command_processor,
                                     bool bindless_resources_used)
    : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      bindless_resources_used_(bindless_resources_used) {}

D3D12TextureCache::~D3D12TextureCache() {
  // Textures must be destroyed while the descriptor cache exists, as ~D3D12Texture uses it.
  DestroyAllTextures(true);

  // Buffers are released before the heaps to detach them.
  for (std::unique_ptr<ScaledResolveVirtualBuffer>& scaled_resolve_buffer_ptr :
       scaled_resolve_2gb_buffers_) {
    scaled_resolve_buffer_ptr.reset();
  }
  scaled_resolve_heaps_.clear();
  REX_PROFILE_PLOT("gpu/texture_cache/scaled_resolve_buffer_used_mb", 0);
}

bool D3D12TextureCache::Initialize() {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  if (IsDrawResolutionScaled()) {
    // No aliasing barriers are needed for ownership changes while the buffers are unused.
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
    REX_ASSERT(scaled_resolve_heaps_.empty());
    u64 scaled_resolve_address_space_size =
        static_cast<u64>(SharedMemory::kBufferSize) *
        (u64{GetDrawResolutionScaleX()} * GetDrawResolutionScaleY());
    scaled_resolve_heaps_.resize(
        static_cast<size_t>(scaled_resolve_address_space_size >> kScaledResolveHeapSizeLog2));
  }
  scaled_resolve_heap_count_ = 0;

  D3D12_ROOT_PARAMETER root_parameters[3];
  // Parameter 0 is constants, changed multiple times when untiling.
  root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  root_parameters[0].Constants.ShaderRegister = 0;
  root_parameters[0].Constants.RegisterSpace = 0;
  root_parameters[0].Constants.Num32BitValues = sizeof(LoadConstants) / sizeof(u32);
  root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 1 is the source, changed multiple times for the same destination.
  D3D12_DESCRIPTOR_RANGE root_dest_range;
  root_dest_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  root_dest_range.NumDescriptors = 1;
  root_dest_range.BaseShaderRegister = 0;
  root_dest_range.RegisterSpace = 0;
  root_dest_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[1].DescriptorTable.pDescriptorRanges = &root_dest_range;
  root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  // Parameter 2 is the destination.
  D3D12_DESCRIPTOR_RANGE root_source_range;
  root_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  root_source_range.NumDescriptors = 1;
  root_source_range.BaseShaderRegister = 0;
  root_source_range.RegisterSpace = 0;
  root_source_range.OffsetInDescriptorsFromTableStart = 0;
  root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
  root_parameters[2].DescriptorTable.pDescriptorRanges = &root_source_range;
  root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc;
  root_signature_desc.NumParameters = UINT(rex::CountOf(root_parameters));
  root_signature_desc.pParameters = root_parameters;
  root_signature_desc.NumStaticSamplers = 0;
  root_signature_desc.pStaticSamplers = nullptr;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(load_root_signature_.ReleaseAndGetAddressOf()) =
      gpu::d3d12::objects::CreateRootSignature(provider, root_signature_desc);
  if (!load_root_signature_) {
    REX_FAIL(gpu, false,
             "D3D12TextureCache: Failed to create the texture loading root "
             "signature");
  }

  D3D12_SHADER_BYTECODE load_shader_code[kLoadShaderCount] = {};
  load_shader_code[kLoadShaderIndex8bpb] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoad8BPBCS, sizeof(shaders::kTextureLoad8BPBCS)};
  load_shader_code[kLoadShaderIndex16bpb] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoad16BPBCS, sizeof(shaders::kTextureLoad16BPBCS)};
  load_shader_code[kLoadShaderIndex32bpb] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoad32BPBCS, sizeof(shaders::kTextureLoad32BPBCS)};
  load_shader_code[kLoadShaderIndex64bpb] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoad64BPBCS, sizeof(shaders::kTextureLoad64BPBCS)};
  load_shader_code[kLoadShaderIndex128bpb] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoad128BPBCS, sizeof(shaders::kTextureLoad128BPBCS)};
  load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadR5G5B5A1B5G5R5A1CS, sizeof(shaders::kTextureLoadR5G5B5A1B5G5R5A1CS)};
  load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadR5G6B5B5G6R5CS, sizeof(shaders::kTextureLoadR5G6B5B5G6R5CS)};
  load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadR5G5B6B5G6R5SwizzleRBGACS,
                            sizeof(shaders::kTextureLoadR5G5B6B5G6R5SwizzleRBGACS)};
  load_shader_code[kLoadShaderIndexRGBA4ToBGRA4] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadR4G4B4A4B4G4R4A4CS, sizeof(shaders::kTextureLoadR4G4B4A4B4G4R4A4CS)};
  load_shader_code[kLoadShaderIndexGBGR8ToGRGB8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadGBGR8GRGB8CS, sizeof(shaders::kTextureLoadGBGR8GRGB8CS)};
  load_shader_code[kLoadShaderIndexGBGR8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadGBGR8RGB8CS, sizeof(shaders::kTextureLoadGBGR8RGB8CS)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGBG8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadBGRG8RGBG8CS, sizeof(shaders::kTextureLoadBGRG8RGBG8CS)};
  load_shader_code[kLoadShaderIndexBGRG8ToRGB8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadBGRG8RGB8CS, sizeof(shaders::kTextureLoadBGRG8RGB8CS)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadR10G11B11RGBA16CS, sizeof(shaders::kTextureLoadR10G11B11RGBA16CS)};
  load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadR10G11B11RGBA16SNormCS,
                            sizeof(shaders::kTextureLoadR10G11B11RGBA16SNormCS)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadR11G11B10RGBA16CS, sizeof(shaders::kTextureLoadR11G11B10RGBA16CS)};
  load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadR11G11B10RGBA16SNormCS,
                            sizeof(shaders::kTextureLoadR11G11B10RGBA16SNormCS)};
  load_shader_code[kLoadShaderIndexDXT1ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDXT1RGBA8CS, sizeof(shaders::kTextureLoadDXT1RGBA8CS)};
  load_shader_code[kLoadShaderIndexDXT3ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDXT3RGBA8CS, sizeof(shaders::kTextureLoadDXT3RGBA8CS)};
  load_shader_code[kLoadShaderIndexDXT5ToRGBA8] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDXT5RGBA8CS, sizeof(shaders::kTextureLoadDXT5RGBA8CS)};
  load_shader_code[kLoadShaderIndexDXNToRG8] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadDXNRG8CS, sizeof(shaders::kTextureLoadDXNRG8CS)};
  load_shader_code[kLoadShaderIndexDXT3A] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadDXT3ACS, sizeof(shaders::kTextureLoadDXT3ACS)};
  load_shader_code[kLoadShaderIndexDXT3AAs1111ToBGRA4] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDXT3AAS1111BGRA4CS, sizeof(shaders::kTextureLoadDXT3AAS1111BGRA4CS)};
  load_shader_code[kLoadShaderIndexDXT5AToR8] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadDXT5AR8CS, sizeof(shaders::kTextureLoadDXT5AR8CS)};
  load_shader_code[kLoadShaderIndexCTX1] =
      D3D12_SHADER_BYTECODE{shaders::kTextureLoadCTX1CS, sizeof(shaders::kTextureLoadCTX1CS)};
  load_shader_code[kLoadShaderIndexDepthUnorm] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDepthUNormCS, sizeof(shaders::kTextureLoadDepthUNormCS)};
  load_shader_code[kLoadShaderIndexDepthFloat] = D3D12_SHADER_BYTECODE{
      shaders::kTextureLoadDepthFloatCS, sizeof(shaders::kTextureLoadDepthFloatCS)};
  D3D12_SHADER_BYTECODE load_shader_code_scaled[kLoadShaderCount] = {};
  if (IsDrawResolutionScaled()) {
    load_shader_code_scaled[kLoadShaderIndex8bpb] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoad8BPBScaledCS, sizeof(shaders::kTextureLoad8BPBScaledCS)};
    load_shader_code_scaled[kLoadShaderIndex16bpb] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoad16BPBScaledCS, sizeof(shaders::kTextureLoad16BPBScaledCS)};
    load_shader_code_scaled[kLoadShaderIndex32bpb] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoad32BPBScaledCS, sizeof(shaders::kTextureLoad32BPBScaledCS)};
    load_shader_code_scaled[kLoadShaderIndex64bpb] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoad64BPBScaledCS, sizeof(shaders::kTextureLoad64BPBScaledCS)};
    load_shader_code_scaled[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoad128BPBScaledCS, sizeof(shaders::kTextureLoad128BPBScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR5G5B5A1B5G5R5A1ScaledCS,
                              sizeof(shaders::kTextureLoadR5G5B5A1B5G5R5A1ScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR5G6B5ToB5G6R5] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR5G6B5B5G6R5ScaledCS,
                              sizeof(shaders::kTextureLoadR5G6B5B5G6R5ScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR5G5B6B5G6R5SwizzleRBGAScaledCS,
                              sizeof(shaders::kTextureLoadR5G5B6B5G6R5SwizzleRBGAScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexRGBA4ToBGRA4] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR4G4B4A4B4G4R4A4ScaledCS,
                              sizeof(shaders::kTextureLoadR4G4B4A4B4G4R4A4ScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR10G11B11RGBA16ScaledCS,
                              sizeof(shaders::kTextureLoadR10G11B11RGBA16ScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR10G11B11RGBA16SNormScaledCS,
                              sizeof(shaders::kTextureLoadR10G11B11RGBA16SNormScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR11G11B10RGBA16ScaledCS,
                              sizeof(shaders::kTextureLoadR11G11B10RGBA16ScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::kTextureLoadR11G11B10RGBA16SNormScaledCS,
                              sizeof(shaders::kTextureLoadR11G11B10RGBA16SNormScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexDepthUnorm] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoadDepthUNormScaledCS, sizeof(shaders::kTextureLoadDepthUNormScaledCS)};
    load_shader_code_scaled[kLoadShaderIndexDepthFloat] = D3D12_SHADER_BYTECODE{
        shaders::kTextureLoadDepthFloatScaledCS, sizeof(shaders::kTextureLoadDepthFloatScaledCS)};
  }

  for (size_t i = 0; i < kLoadShaderCount; ++i) {
    const D3D12_SHADER_BYTECODE& current_load_shader_code = load_shader_code[i];
    if (!current_load_shader_code.pShaderBytecode) {
      continue;
    }
    *(load_pipelines_[i].ReleaseAndGetAddressOf()) = gpu::d3d12::objects::CreateComputePipeline(
        device, current_load_shader_code.pShaderBytecode, current_load_shader_code.BytecodeLength,
        load_root_signature_.Get());
    if (!load_pipelines_[i]) {
      REX_FAIL(gpu, false,
               "D3D12TextureCache: Failed to create the texture loading pipeline "
               "for shader {}",
               i);
    }
    if (IsDrawResolutionScaled()) {
      const D3D12_SHADER_BYTECODE& current_load_shader_code_scaled = load_shader_code_scaled[i];
      if (current_load_shader_code_scaled.pShaderBytecode) {
        *(load_pipelines_scaled_[i].ReleaseAndGetAddressOf()) =
            gpu::d3d12::objects::CreateComputePipeline(
                device, current_load_shader_code_scaled.pShaderBytecode,
                current_load_shader_code_scaled.BytecodeLength, load_root_signature_.Get());
        if (!load_pipelines_scaled_[i]) {
          REX_FAIL(gpu, false,
                   "D3D12TextureCache: Failed to create the resolution-scaled "
                   "texture loading pipeline for shader {}",
                   i);
        }
      }
    }
  }

  srv_descriptor_cache_allocated_ = 0;

  // Copying a null descriptor is faster than creating an SRV.
  D3D12_DESCRIPTOR_HEAP_DESC null_srv_descriptor_heap_desc;
  null_srv_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  null_srv_descriptor_heap_desc.NumDescriptors = std::to_underlying(NullSRVDescriptorIndex::kCount);
  null_srv_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  null_srv_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&null_srv_descriptor_heap_desc,
                                          IID_PPV_ARGS(&null_srv_descriptor_heap_)))) {
    REX_FAIL(gpu, false,
             "D3D12TextureCache: Failed to create the descriptor heap for null "
             "SRVs");
  }
  null_srv_descriptor_heap_start_ = null_srv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
  null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
      D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  null_srv_desc.Texture2DArray.MostDetailedMip = 0;
  null_srv_desc.Texture2DArray.MipLevels = 1;
  null_srv_desc.Texture2DArray.FirstArraySlice = 0;
  null_srv_desc.Texture2DArray.ArraySize = 1;
  null_srv_desc.Texture2DArray.PlaneSlice = 0;
  null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    std::to_underlying(NullSRVDescriptorIndex::k2DArray)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  null_srv_desc.Texture3D.MostDetailedMip = 0;
  null_srv_desc.Texture3D.MipLevels = 1;
  null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    std::to_underlying(NullSRVDescriptorIndex::k3D)));
  null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  null_srv_desc.TextureCube.MostDetailedMip = 0;
  null_srv_desc.TextureCube.MipLevels = 1;
  null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
  device->CreateShaderResourceView(
      nullptr, &null_srv_desc,
      provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                    std::to_underlying(NullSRVDescriptorIndex::kCube)));

  return true;
}

void D3D12TextureCache::ClearCache() {
  TextureCache::ClearCache();

  srv_descriptor_cache_free_.clear();
  srv_descriptor_cache_free_count_ = 0;
  srv_descriptor_cache_allocated_ = 0;
  srv_descriptor_cache_.clear();
}

void D3D12TextureCache::BeginSubmission(u64 new_submission_index) {
  TextureCache::BeginSubmission(new_submission_index);

  // ExecuteCommandLists is a full UAV and aliasing barrier.
  if (IsDrawResolutionScaled()) {
    size_t scaled_resolve_buffer_count = GetScaledResolveBufferCount();
    for (size_t i = 0; i < scaled_resolve_buffer_count; ++i) {
      ScaledResolveVirtualBuffer* scaled_resolve_buffer = scaled_resolve_2gb_buffers_[i].get();
      if (scaled_resolve_buffer) {
        scaled_resolve_buffer->ClearUAVBarrierPending();
      }
    }
    std::memset(scaled_resolve_1gb_buffer_indices_, UINT8_MAX,
                sizeof(scaled_resolve_1gb_buffer_indices_));
  }
}

void D3D12TextureCache::BeginFrame() {
  TextureCache::BeginFrame();

  std::memset(unsupported_format_features_used_, 0, sizeof(unsupported_format_features_used_));
}

void D3D12TextureCache::EndFrame() {
  bool unsupported_header_written = false;
  for (u32 i = 0; i < 64; ++i) {
    u32 unsupported_features = unsupported_format_features_used_[i];
    if (unsupported_features == 0) {
      continue;
    }
    if (!unsupported_header_written) {
      REXLOG_ERROR(gpu, "Unsupported texture formats used in the frame:");
      unsupported_header_written = true;
    }
    REXLOG_ERROR(gpu, "* {}{}{}{}", xenos::FormatInfo::Get(xenos::TextureFormat(i))->name,
                 unsupported_features & kUnsupportedResourceBit ? " resource" : "",
                 unsupported_features & kUnsupportedUnormBit ? " unsigned" : "",
                 unsupported_features & kUnsupportedSnormBit ? " signed" : "");
    unsupported_format_features_used_[i] = 0;
  }
}

void D3D12TextureCache::RequestTextures(u32 used_texture_mask) {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
  REX_PROFILE_SCOPE("gpu");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES

  TextureCache::RequestTextures(used_texture_mask);

  // Wrapper loading may bind compute pipelines, so 3D as 2D wrappers are created here.
  if (REXCVAR_GET(::rex::gpu::cvars::texture_3d_to_2d)) {
    u32 textures_3d = used_texture_mask;
    u32 index_3d;
    while (rex::bit_scan_forward(textures_3d, &index_3d)) {
      textures_3d &= ~(static_cast<u32>(1) << index_3d);
      const TextureBinding* binding = GetValidTextureBinding(index_3d);
      if (!binding || binding->key.dimension != xenos::DataDimension::k3D) {
        continue;
      }
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        texture->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
      D3D12Texture* texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
      if (texture_signed) {
        texture_signed->GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      }
    }
  }

  // Textures always use both read states, as the same texture may be bound to VS and PS.
  u32 textures_remaining = used_texture_mask;
  u32 index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(static_cast<u32>(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    D3D12Texture* binding_texture = static_cast<D3D12Texture*>(binding->texture);
    if (binding_texture != nullptr) {
      binding_texture->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture->GetResource(),
          binding_texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    D3D12Texture* binding_texture_signed = static_cast<D3D12Texture*>(binding->texture_signed);
    if (binding_texture_signed != nullptr) {
      binding_texture_signed->MarkAsUsed();
      command_processor_.PushTransitionBarrier(
          binding_texture_signed->GetResource(),
          binding_texture_signed->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
  }
}

bool D3D12TextureCache::AreActiveTextureSRVKeysUpToDate(
    const TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    const TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      if (key.key.is_valid) {
        return false;
      }
      continue;
    }
    if (key.key != binding->key || key.host_swizzle != binding->host_swizzle ||
        key.swizzled_signs != binding->swizzled_signs) {
      return false;
    }
  }
  return true;
}

void D3D12TextureCache::WriteActiveTextureSRVKeys(
    TextureSRVKey* keys, const D3D12Shader::TextureBinding* host_shader_bindings,
    size_t host_shader_binding_count) const {
  for (size_t i = 0; i < host_shader_binding_count; ++i) {
    TextureSRVKey& key = keys[i];
    const TextureBinding* binding = GetValidTextureBinding(host_shader_bindings[i].fetch_constant);
    if (!binding) {
      key.key.Invalidate();
      key.host_swizzle = xenos::REXGPU_SWIZZLE_0000;
      key.swizzled_signs = kSwizzledSignsUnsigned;
      continue;
    }
    key.key = binding->key;
    key.host_swizzle = binding->host_swizzle;
    key.swizzled_signs = binding->swizzled_signs;
  }
}

void D3D12TextureCache::WriteActiveTextureBindfulSRV(
    const D3D12Shader::TextureBinding& host_shader_binding, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  REX_ASSERT(!bindless_resources_used_);
  u32 descriptor_index = UINT32_MAX;
  Texture* texture = nullptr;
  u32 fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (host_shader_binding.is_signed) {
      // Signed compressed textures (DXN and DXT5A) are not supported.
      if (xenos::IsAnySignSigned(binding->swizzled_signs)) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked, true,
                                                           binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index_signed;
        }
      }
    } else {
      if (xenos::IsAnySignNotSigned(binding->swizzled_signs)) {
        texture = binding->texture;
        if (force_special_view && texture) {
          descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                           xenos::DataDimension::k2DOrStacked,
                                                           false, binding->host_swizzle);
        } else {
          descriptor_index = d3d12_binding.descriptor_index;
        }
      }
    }
  }
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_CPU_DESCRIPTOR_HANDLE source_handle;
  if (descriptor_index != UINT32_MAX) {
    REX_ASSERT(texture != nullptr);
    texture->MarkAsUsed();
    source_handle = GetTextureDescriptorCPUHandle(descriptor_index);
  } else {
    NullSRVDescriptorIndex null_descriptor_index;
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        null_descriptor_index = NullSRVDescriptorIndex::k3D;
        break;
      case xenos::FetchOpDimension::kCube:
        null_descriptor_index = NullSRVDescriptorIndex::kCube;
        break;
      default:
        REX_ASSERT(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                   host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        null_descriptor_index = NullSRVDescriptorIndex::k2DArray;
    }
    source_handle = provider.OffsetViewDescriptor(null_srv_descriptor_heap_start_,
                                                  static_cast<u32>(null_descriptor_index));
  }
  auto device = provider.GetDevice();
  {
#if REXGPU_FINE_GRAINED_DRAW_SCOPES
    REX_PROFILE_SCOPE_TEXT("gpu",
                           "rex::gpu::d3d12::D3D12TextureCache::WriteActiveTextureBindfulSRV->"
                           "CopyDescriptorsSimple");
#endif  // REXGPU_FINE_GRAINED_DRAW_SCOPES
    device->CopyDescriptorsSimple(1, handle, source_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
}

u32 D3D12TextureCache::GetActiveTextureBindlessSRVIndex(
    const D3D12Shader::TextureBinding& host_shader_binding) {
  REX_ASSERT(bindless_resources_used_);
  u32 descriptor_index = UINT32_MAX;
  u32 fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding && AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                              (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                               host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
    const D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[fetch_constant_index];
    if (force_special_view) {
      Texture* texture = nullptr;
      bool use_signed =
          host_shader_binding.is_signed && xenos::IsAnySignSigned(binding->swizzled_signs);
      if (use_signed) {
        texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                 : binding->texture;
      } else {
        texture = binding->texture;
      }
      if (texture) {
        descriptor_index = FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(texture),
                                                         xenos::DataDimension::k2DOrStacked,
                                                         use_signed, binding->host_swizzle);
      }
    } else {
      descriptor_index = host_shader_binding.is_signed ? d3d12_binding.descriptor_index_signed
                                                       : d3d12_binding.descriptor_index;
    }
  }
  if (descriptor_index == UINT32_MAX) {
    switch (host_shader_binding.dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        descriptor_index =
            std::to_underlying(D3D12CommandProcessor::SystemBindlessView::kNullTexture3D);
        break;
      case xenos::FetchOpDimension::kCube:
        descriptor_index =
            std::to_underlying(D3D12CommandProcessor::SystemBindlessView::kNullTextureCube);
        break;
      default:
        REX_ASSERT(host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                   host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
        descriptor_index =
            std::to_underlying(D3D12CommandProcessor::SystemBindlessView::kNullTexture2DArray);
    }
  }
  return descriptor_index;
}

D3D12TextureCache::SamplerParameters D3D12TextureCache::GetSamplerParameters(
    const D3D12Shader::SamplerBinding& binding) const {
  const auto& regs = GetRegisterFile();
  xenos::TextureFetch fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;

  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  xenos::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  u32 mip_min_level, mip_max_level;
  xenos::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                          &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;

  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  i32 anisotropic_override = REXCVAR_GET(::rex::gpu::cvars::anisotropic_override);
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax161);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;

  return parameters;
}

void D3D12TextureCache::WriteSampler(SamplerParameters parameters,
                                     D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  D3D12_SAMPLER_DESC desc;
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (static_cast<u32>(parameters.aniso_filter) - 1);
  } else {
    D3D12_FILTER_TYPE d3d_filter_min =
        parameters.min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mag =
        parameters.mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE d3d_filter_mip =
        parameters.mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(d3d_filter_min, d3d_filter_mag, d3d_filter_mip,
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  static const D3D12_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D12_TEXTURE_ADDRESS_MODE_WRAP,
      /* kMirroredRepeat       */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
      /* kClampToEdge          */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      /* kMirrorClampToEdge    */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      // No Direct3D 12 equivalent of GL_CLAMP (clamp to half edge, half border).
      /* kClampToHalfway       */ D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      // No Direct3D 12 equivalent of mirror and clamp to border.
      /* kMirrorClampToHalfway */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
      /* kClampToBorder        */ D3D12_TEXTURE_ADDRESS_MODE_BORDER,
      /* kMirrorClampToBorder  */ D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[static_cast<u32>(parameters.clamp_x)];
  desc.AddressV = kAddressModeMap[static_cast<u32>(parameters.clamp_y)];
  desc.AddressW = kAddressModeMap[static_cast<u32>(parameters.clamp_z)];
  // LOD biasing is performed in shaders.
  desc.MipLODBias = 0.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      REX_ASSERT(parameters.border_color == xenos::BorderColor::k_ABGR_Black);
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  desc.MinLOD = static_cast<f32>(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      REX_ASSERT(!parameters.mip_linear);
      desc.MaxLOD += 0.25f;
    }
  } else {
    desc.MaxLOD = FLT_MAX;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  device->CreateSampler(&desc, handle);
}

bool D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
    u32& scale_x, u32& scale_y, const gpu::d3d12::D3D12Provider& provider) {
  bool was_clamped;
  if (provider.GetTiledResourcesTier() < D3D12_TILED_RESOURCES_TIER_1) {
    was_clamped = scale_x > 1 || scale_y > 1;
    scale_x = 1;
    scale_y = 1;
    return !was_clamped;
  }
  was_clamped = false;
  u32 virtual_address_bits_per_resource = provider.GetVirtualAddressBitsPerResource();
  while (scale_x > 1 || scale_y > 1) {
    u64 highest_scaled_address =
        static_cast<u64>(SharedMemory::kBufferSize) * (u64{scale_x} * scale_y) - 1;
    if (static_cast<u32>(64) - rex::lzcnt(highest_scaled_address) <=
        virtual_address_bits_per_resource) {
      break;
    }
    // Reduce horizontal resolution first, as vertical differences show more in perspective.
    was_clamped = true;
    if (scale_x >= scale_y) {
      --scale_x;
    } else {
      --scale_y;
    }
  }
  return !was_clamped;
}

bool D3D12TextureCache::EnsureScaledResolveMemoryCommitted(u32 start_unscaled, u32 length_unscaled,
                                                           u32 length_scaled_alignment_log2) {
  REX_ASSERT(IsDrawResolutionScaled());

  if (length_unscaled == 0) {
    return true;
  }
  if (start_unscaled > SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  u32 draw_resolution_scale_area = GetDrawResolutionScaleX() * GetDrawResolutionScaleY();
  u64 first_scaled = static_cast<u64>(start_unscaled) * draw_resolution_scale_area;
  u64 length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  u64 last_scaled =
      (static_cast<u64>(start_unscaled + (length_unscaled - 1)) * draw_resolution_scale_area +
       length_scaled_alignment_bits) &
      ~length_scaled_alignment_bits;

  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  // Buffers for the whole range are created before the heaps, so new ones have no mapped heaps.
  std::array<size_t, 2> possible_buffers_first =
      GetPossibleScaledResolveBufferIndices(first_scaled);
  std::array<size_t, 2> possible_buffers_last = GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_first = std::min(possible_buffers_first[0], possible_buffers_first[1]);
  size_t possible_buffer_last = std::max(possible_buffers_last[0], possible_buffers_last[1]);
  for (size_t i = possible_buffer_first; i <= possible_buffer_last; ++i) {
    if (scaled_resolve_2gb_buffers_[i]) {
      continue;
    }
    D3D12_RESOURCE_DESC scaled_resolve_buffer_desc;
    // Buffer indices are gigabytes.
    gpu::d3d12::objects::FillBufferResourceDesc(
        scaled_resolve_buffer_desc,
        std::min(static_cast<u64>(1) << 31,
                 static_cast<u64>(SharedMemory::kBufferSize) * draw_resolution_scale_area -
                     (static_cast<u64>(i) << 30)),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    constexpr D3D12_RESOURCE_STATES kScaledResolveVirtualBufferInitialState =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* scaled_resolve_buffer_resource;
    if (FAILED(device->CreateReservedResource(&scaled_resolve_buffer_desc,
                                              kScaledResolveVirtualBufferInitialState, nullptr,
                                              IID_PPV_ARGS(&scaled_resolve_buffer_resource)))) {
      REX_FAIL(gpu, false,
               "D3D12TextureCache: Failed to create a 2 GB tiled buffer for draw "
               "resolution scaling");
    }
    scaled_resolve_2gb_buffers_[i] = std::make_unique<ScaledResolveVirtualBuffer>(
        scaled_resolve_buffer_resource, kScaledResolveVirtualBufferInitialState);
    scaled_resolve_buffer_resource->Release();
  }

  u32 heap_first = static_cast<u32>(first_scaled >> kScaledResolveHeapSizeLog2);
  u32 heap_last = static_cast<u32>(last_scaled >> kScaledResolveHeapSizeLog2);
  for (u32 i = heap_first; i <= heap_last; ++i) {
    if (scaled_resolve_heaps_[i]) {
      continue;
    }
    auto direct_queue = provider.GetDirectQueue();
    const D3D12_HEAP_DESC heap_desc = objects::BuildDefaultHeapDesc(
        kScaledResolveHeapSize,
        D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed());
    Microsoft::WRL::ComPtr<ID3D12Heap> scaled_resolve_heap;
    if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&scaled_resolve_heap)))) {
      REX_FAIL(gpu, false, "D3D12TextureCache: Failed to create a scaled resolve tile heap");
    }
    scaled_resolve_heaps_[i] = scaled_resolve_heap;
    ++scaled_resolve_heap_count_;
    REX_PROFILE_PLOT("gpu/texture_cache/scaled_resolve_buffer_used_mb",
                     scaled_resolve_heap_count_ << (kScaledResolveHeapSizeLog2 - 20));
    D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
    region_start_coordinates.Y = 0;
    region_start_coordinates.Z = 0;
    region_start_coordinates.Subresource = 0;
    D3D12_TILE_REGION_SIZE region_size;
    region_size.NumTiles = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    region_size.UseBox = FALSE;
    D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
    UINT heap_range_start_offset = 0;
    UINT range_tile_count = kScaledResolveHeapSize / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    std::array<size_t, 2> buffer_indices =
        GetPossibleScaledResolveBufferIndices(static_cast<u64>(i) << kScaledResolveHeapSizeLog2);
    for (size_t j = 0; j < 2; ++j) {
      size_t buffer_index = buffer_indices[j];
      if (j && buffer_index == buffer_indices[0]) {
        break;
      }
      region_start_coordinates.X = UINT(((static_cast<u64>(i) << kScaledResolveHeapSizeLog2) -
                                         (static_cast<u64>(buffer_index) << 30)) /
                                        D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
      direct_queue->UpdateTileMappings(
          scaled_resolve_2gb_buffers_[buffer_index]->GetResource(), 1, &region_start_coordinates,
          &region_size, scaled_resolve_heap.Get(), 1, &range_flags, &heap_range_start_offset,
          &range_tile_count, D3D12_TILE_MAPPING_FLAG_NONE);
    }
    command_processor_.NotifyQueueOperationsDoneDirectly();
  }
  return true;
}

bool D3D12TextureCache::UpdateScaledResolveRange(u32 start_unscaled, u32 length_unscaled,
                                                 u32 length_scaled_alignment_log2) {
  REX_ASSERT(IsDrawResolutionScaled());

  if (!length_unscaled || start_unscaled >= SharedMemory::kBufferSize ||
      (SharedMemory::kBufferSize - start_unscaled) < length_unscaled) {
    return false;
  }

  u32 draw_resolution_scale_area = GetDrawResolutionScaleX() * GetDrawResolutionScaleY();
  u64 start_scaled = static_cast<u64>(start_unscaled) * draw_resolution_scale_area;
  u64 length_scaled_alignment_bits = (UINT64_C(1) << length_scaled_alignment_log2) - 1;
  u64 length_scaled = (static_cast<u64>(length_unscaled) * draw_resolution_scale_area +
                       length_scaled_alignment_bits) &
                      ~length_scaled_alignment_bits;
  u64 last_scaled = start_scaled + (length_scaled - 1);

  std::array<size_t, 2> possible_buffer_indices_first =
      GetPossibleScaledResolveBufferIndices(start_scaled);
  std::array<size_t, 2> possible_buffer_indices_last =
      GetPossibleScaledResolveBufferIndices(last_scaled);
  size_t possible_buffer_indices_common[2];
  size_t possible_buffer_indices_common_count = 0;
  for (size_t i = 0; i <= static_cast<size_t>(possible_buffer_indices_first[0] !=
                                              possible_buffer_indices_first[1]);
       ++i) {
    size_t possible_buffer_index_first = possible_buffer_indices_first[i];
    for (size_t j = 0; j <= static_cast<size_t>(possible_buffer_indices_last[0] !=
                                                possible_buffer_indices_last[1]);
         ++j) {
      if (possible_buffer_indices_last[j] == possible_buffer_index_first) {
        bool possible_buffer_index_already_added = false;
        for (size_t k = 0; k < possible_buffer_indices_common_count; ++k) {
          if (possible_buffer_indices_common[k] == possible_buffer_index_first) {
            possible_buffer_index_already_added = true;
            break;
          }
        }
        if (!possible_buffer_index_already_added) {
          REX_ASSERT(possible_buffer_indices_common_count < 2);
          possible_buffer_indices_common[possible_buffer_indices_common_count++] =
              possible_buffer_index_first;
        }
      }
    }
  }
  if (!possible_buffer_indices_common_count) {
    return false;
  }

  size_t gigabyte_first = static_cast<size_t>(start_scaled >> 30);
  size_t gigabyte_last = static_cast<size_t>(last_scaled >> 30);

  size_t new_buffer_index;
  if (possible_buffer_indices_common_count >= 2) {
    // Prefer the buffer in use to need fewer aliasing barriers.
    REX_ASSERT(gigabyte_first + 1 >= gigabyte_last);
    size_t possible_buffer_indices_already_used[2] = {};
    for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
      size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
      for (size_t j = 0; j < possible_buffer_indices_common_count; ++j) {
        if (possible_buffer_indices_common[j] == gigabyte_current_buffer_index) {
          ++possible_buffer_indices_already_used[j];
        }
      }
    }
    new_buffer_index = possible_buffer_indices_common[static_cast<size_t>(
        possible_buffer_indices_already_used[1] > possible_buffer_indices_already_used[0])];
  } else {
    new_buffer_index = possible_buffer_indices_common[0];
  }

  const ScaledResolveVirtualBuffer* new_buffer =
      scaled_resolve_2gb_buffers_[new_buffer_index].get();
  REX_ASSERT(new_buffer != nullptr);
  ID3D12Resource* new_buffer_resource = new_buffer->GetResource();
  for (size_t i = gigabyte_first; i <= gigabyte_last; ++i) {
    size_t gigabyte_current_buffer_index = scaled_resolve_1gb_buffer_indices_[i];
    if (gigabyte_current_buffer_index == new_buffer_index) {
      continue;
    }
    if (gigabyte_current_buffer_index != SIZE_MAX) {
      ScaledResolveVirtualBuffer* gigabyte_current_buffer =
          scaled_resolve_2gb_buffers_[gigabyte_current_buffer_index].get();
      REX_ASSERT(gigabyte_current_buffer != nullptr);
      command_processor_.PushAliasingBarrier(gigabyte_current_buffer->GetResource(),
                                             new_buffer_resource);
      // An aliasing barrier synchronizes and flushes everything.
      gigabyte_current_buffer->ClearUAVBarrierPending();
    }
    scaled_resolve_1gb_buffer_indices_[i] = new_buffer_index;
  }

  scaled_resolve_current_range_start_scaled_ = start_scaled;
  scaled_resolve_current_range_length_scaled_ = length_scaled;
  return true;
}

void D3D12TextureCache::TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATES new_state) {
  REX_ASSERT(IsDrawResolutionScaled());
  ScaledResolveVirtualBuffer& buffer = GetCurrentScaledResolveBuffer();
  command_processor_.PushTransitionBarrier(buffer.GetResource(), buffer.SetResourceState(new_state),
                                           new_state);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2SRV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 element_size_bytes_pow2) {
  REX_ASSERT(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  REX_ASSERT(buffer != nullptr);
  gpu::d3d12::objects::CreateBufferTypedSRV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->GetResource(),
      gpu::d3d12::objects::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      static_cast<u32>(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (static_cast<u64>(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

void D3D12TextureCache::CreateCurrentScaledResolveRangeUintPow2UAV(
    D3D12_CPU_DESCRIPTOR_HANDLE handle, u32 element_size_bytes_pow2) {
  REX_ASSERT(IsDrawResolutionScaled());
  size_t buffer_index = GetCurrentScaledResolveBufferIndex();
  const ScaledResolveVirtualBuffer* buffer = scaled_resolve_2gb_buffers_[buffer_index].get();
  REX_ASSERT(buffer != nullptr);
  gpu::d3d12::objects::CreateBufferTypedUAV(
      command_processor_.GetD3D12Provider().GetDevice(), handle, buffer->GetResource(),
      gpu::d3d12::objects::GetUintPow2DXGIFormat(element_size_bytes_pow2),
      static_cast<u32>(scaled_resolve_current_range_length_scaled_ >> element_size_bytes_pow2),
      (scaled_resolve_current_range_start_scaled_ - (static_cast<u64>(buffer_index) << 30)) >>
          element_size_bytes_pow2);
}

ID3D12Resource* D3D12TextureCache::RequestSwapTexture(D3D12_SHADER_RESOURCE_VIEW_DESC& srv_desc_out,
                                                      xenos::TextureFormat& format_out,
                                                      u32* width_unscaled_out,
                                                      u32* height_unscaled_out) {
  const auto& regs = GetRegisterFile();
  xenos::TextureFetch fetch = regs.GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 || key.dimension != xenos::DataDimension::k2DOrStacked) {
    return nullptr;
  }
  D3D12Texture* texture = static_cast<D3D12Texture*>(FindOrCreateTexture(key));
  if (texture == nullptr || !LoadTextureData(*texture)) {
    return nullptr;
  }
  texture->MarkAsUsed();
  // Presentation reads the swap texture in compute, so only NON_PIXEL_SHADER_RESOURCE is needed.
  ID3D12Resource* texture_resource = texture->GetResource();
  command_processor_.PushTransitionBarrier(
      texture_resource, texture->SetResourceState(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  srv_desc_out.Format = GetDXGIUnormFormat(key);
  srv_desc_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc_out.Shader4ComponentMapping =
      GuestToHostSwizzle(fetch.swizzle, GetHostFormatSwizzle(key)) |
      D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
  srv_desc_out.Texture2D.MostDetailedMip = 0;
  srv_desc_out.Texture2D.MipLevels = 1;
  srv_desc_out.Texture2D.PlaneSlice = 0;
  srv_desc_out.Texture2D.ResourceMinLODClamp = 0.0f;
  // Only texture->key, not BindingInfoFromFetchConstant, says whether the texture is scaled.
  key = texture->GetKey();
  if (width_unscaled_out) {
    *width_unscaled_out = key.GetWidth();
  }
  if (height_unscaled_out) {
    *height_unscaled_out = key.GetHeight();
  }
  format_out = key.format;
  return texture_resource;
}

D3D12TextureCache::D3D12Texture::D3D12Texture(D3D12TextureCache& texture_cache,
                                              const TextureKey& key, ID3D12Resource* resource,
                                              D3D12_RESOURCE_STATES resource_state,
                                              bool track_usage)
    : Texture(texture_cache, key, track_usage),
      resource_(resource),
      resource_state_(resource_state) {
  ID3D12Device* device = texture_cache.command_processor_.GetD3D12Provider().GetDevice();
  D3D12_RESOURCE_DESC resource_desc = resource_->GetDesc();
  SetHostMemoryUsage(device->GetResourceAllocationInfo(0, 1, &resource_desc).SizeInBytes);
}

D3D12TextureCache::D3D12Texture::~D3D12Texture() {
  auto& d3d12_texture_cache = static_cast<D3D12TextureCache&>(GetTextureCache());
  for (const auto& descriptor_pair : srv_descriptors_) {
    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);
  }
}

bool D3D12TextureCache::IsDecompressionNeeded(xenos::TextureFormat format, u32 width,
                                              u32 height) const {
  DXGI_FORMAT dxgi_format_uncompressed =
      kHostFormats[static_cast<u32>(format)].dxgi_format_uncompressed;
  if (dxgi_format_uncompressed == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  const xenos::FormatInfo* format_info = xenos::FormatInfo::Get(format);
  if (!(width & (format_info->block_width - 1)) && !(height & (format_info->block_height - 1))) {
    return false;
  }
  // UnalignedBlockTexturesSupported covers 4x4 block compressed textures, not 2x1 subsampled ones.
  if (format_info->block_width == 4 && format_info->block_height == 4 &&
      command_processor_.GetD3D12Provider().AreUnalignedBlockTexturesSupported()) {
    return false;
  }
  return true;
}

TextureCache::LoadShaderIndex D3D12TextureCache::GetLoadShaderIndex(TextureKey key) const {
  const HostFormat& host_format = kHostFormats[static_cast<u32>(key.format)];
  if (key.signed_separate) {
    return host_format.load_shader_signed;
  }
  if (IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
    return host_format.load_shader_decompress;
  }
  return host_format.load_shader;
}

bool D3D12TextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormat& host_format = kHostFormats[static_cast<u32>(key.format)];
  return host_format.load_shader_signed != kLoadShaderIndexUnknown &&
         host_format.load_shader_signed != host_format.load_shader;
}

bool D3D12TextureCache::IsScaledResolveSupportedForFormat(TextureKey key) const {
  LoadShaderIndex load_shader = GetLoadShaderIndex(key);
  return load_shader != kLoadShaderIndexUnknown && load_pipelines_scaled_[load_shader] != nullptr;
}

u32 D3D12TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  // A dense cache line aligned array avoids touching the full HostFormat on every fetch.
  alignas(64) static const auto kSwizzleCache = []() {
    std::array<u16, 64> arr{};
    for (int i = 0; i < 64; ++i) {
      arr[i] = static_cast<u16>(kHostFormats[i].swizzle);
    }
    return arr;
  }();
  return kSwizzleCache[static_cast<u32>(key.format)];
}

u32 D3D12TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    case xenos::DataDimension::k3D:
      return D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURECUBE_DIMENSION;
    default:
      REX_UNHANDLED_CASE(dimension);
      return 0;
  }
}

static_assert(D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION == D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);

u32 D3D12TextureCache::GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
    case xenos::DataDimension::k3D:
      // 1D and 2D are emulated as 2D arrays.
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION;
    case xenos::DataDimension::kCube:
      return D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / 6 * 6;
    default:
      REX_UNHANDLED_CASE(dimension);
      return 0;
  }
}

std::unique_ptr<TextureCache::Texture> D3D12TextureCache::CreateTexture(TextureKey key) {
  D3D12_RESOURCE_DESC desc;
  desc.Format = GetDXGIResourceFormat(key);
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[static_cast<u32>(key.format)] |= kUnsupportedResourceBit;
    return nullptr;
  }
  if (key.dimension == xenos::DataDimension::k3D) {
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  } else {
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  }
  desc.Alignment = 0;
  desc.Width = key.GetWidth();
  desc.Height = key.GetHeight();
  if (key.scaled_resolve) {
    desc.Width *= GetDrawResolutionScaleX();
    desc.Height *= GetDrawResolutionScaleY();
  }
  desc.DepthOrArraySize = key.GetDepthOrArraySize();
  desc.MipLevels = key.mip_max_level + 1;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  // Untiling goes through a buffer rather than unordered access, as copying is infrequent.
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_STATES resource_state = D3D12_RESOURCE_STATE_COPY_DEST;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&gpu::d3d12::objects::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             resource_state, nullptr, IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }
  return std::make_unique<D3D12Texture>(*this, key, resource.Get(), resource_state);
}

bool D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  D3D12Texture& d3d12_texture = static_cast<D3D12Texture&>(texture);
  TextureKey texture_key = d3d12_texture.GetKey();

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();

  LoadShaderIndex load_shader = GetLoadShaderIndex(texture_key);
  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }
  bool texture_resolution_scaled = texture_key.scaled_resolve;
  ID3D12PipelineState* pipeline = texture_resolution_scaled
                                      ? load_pipelines_scaled_[load_shader].Get()
                                      : load_pipelines_[load_shader].Get();
  if (pipeline == nullptr) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  const xenos::TextureGuestLayout& guest_layout = d3d12_texture.GetGuestLayout();
  xenos::DataDimension dimension = texture_key.dimension;
  bool is_3d = dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || d3d12_texture.GetForceLoad3dTiling();
  u32 width = texture_key.GetWidth();
  u32 height = texture_key.GetHeight();
  u32 depth_or_array_size = texture_key.GetDepthOrArraySize();
  u32 depth = is_3d ? depth_or_array_size : 1;
  u32 array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const xenos::FormatInfo* guest_format_info = xenos::FormatInfo::Get(guest_format);
  u32 block_width = guest_format_info->block_width;
  u32 block_height = guest_format_info->block_height;
  u32 bytes_per_block = guest_format_info->GetBytesPerBlock();
  u32 level_first = load_base ? 0 : 1;
  u32 level_last = load_mips ? texture_key.mip_max_level : 0;
  REX_ASSERT(level_first <= level_last);
  u32 level_packed = guest_layout.packed_level;
  u32 level_stored_first = std::min(level_first, level_packed);
  u32 level_stored_last = std::min(level_last, level_packed);
  u32 texture_resolution_scale_x = texture_resolution_scaled ? GetDrawResolutionScaleX() : 1;
  u32 texture_resolution_scale_y = texture_resolution_scaled ? GetDrawResolutionScaleY() : 1;

  // With the packed tail at level 0, iteration 0 is the base tail and 1 is the mips tail.
  u32 loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = static_cast<u32>(level_first != 0);
    loop_level_last = static_cast<u32>(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }

  bool host_block_compressed = kHostFormats[static_cast<u32>(guest_format)].is_block_compressed &&
                               !IsDecompressionNeeded(guest_format, width, height);
  u32 host_block_width = host_block_compressed ? block_width : 1;
  u32 host_block_height = host_block_compressed ? block_height : 1;
  u32 host_x_blocks_per_thread = UINT32_C(1) << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }
  UINT64 copy_buffer_size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_layer_layout_base;
  u64 host_layer_size_base = 0;
  // Indexed like guest stored mips, 1 to min(level_last, level_packed), or 0 if level_packed is 0.
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT
  host_layer_layouts_mips[xenos::kTextureMaxMips];
  UINT64 host_layer_sizes_mips[xenos::kTextureMaxMips];
  DXGI_FORMAT host_copy_format = GetDXGIResourceFormat(guest_format, width, height);
  for (u32 loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    u32 level = (level_packed == 0) ? 0 : loop_level;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_layer_layout =
        is_base ? host_layer_layout_base : host_layer_layouts_mips[level];
    level_host_layer_layout.Offset = copy_buffer_size;
    level_host_layer_layout.Footprint.Format = host_copy_format;
    if (level == level_packed) {
      // Load the whole packed tail to copy regions out of it.
      const xenos::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_host_layer_layout.Footprint.Width = guest_layout_packed.x_extent_blocks * block_width;
      level_host_layer_layout.Footprint.Height = guest_layout_packed.y_extent_blocks * block_height;
      level_host_layer_layout.Footprint.Depth = guest_layout_packed.z_extent;
    } else {
      level_host_layer_layout.Footprint.Width = std::max(width >> level, static_cast<u32>(1));
      level_host_layer_layout.Footprint.Height = std::max(height >> level, static_cast<u32>(1));
      level_host_layer_layout.Footprint.Depth = std::max(depth >> level, static_cast<u32>(1));
    }
    level_host_layer_layout.Footprint.Width =
        rex::round_up_nonzero(level_host_layer_layout.Footprint.Width * texture_resolution_scale_x,
                              UINT(host_block_width));
    level_host_layer_layout.Footprint.Height =
        rex::round_up_nonzero(level_host_layer_layout.Footprint.Height * texture_resolution_scale_y,
                              UINT(host_block_height));
    level_host_layer_layout.Footprint.RowPitch =
        rex::Align(rex::round_up_nonzero(level_host_layer_layout.Footprint.Width / host_block_width,
                                         host_x_blocks_per_thread) *
                       load_shader_info.bytes_per_host_block,
                   static_cast<u32>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    UINT64 level_host_layer_size =
        rex::Align(UINT64(level_host_layer_layout.Footprint.RowPitch) *
                       (level_host_layer_layout.Footprint.Height / host_block_height) *
                       level_host_layer_layout.Footprint.Depth,
                   UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
    (is_base ? host_layer_size_base : host_layer_sizes_mips[level]) = level_host_layer_size;
    copy_buffer_size += level_host_layer_size * array_size;
  }
  D3D12_RESOURCE_STATES copy_buffer_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  ID3D12Resource* copy_buffer = command_processor_.RequestScratchGPUBuffer(
      static_cast<u32>(copy_buffer_size), copy_buffer_state);
  if (copy_buffer == nullptr) {
    return false;
  }

  u32 descriptor_count = 1;
  if (texture_resolution_scaled) {
    descriptor_count += (level_first == 0 && level_last != 0) ? 2 : 1;
  } else {
    if (!bindless_resources_used_) {
      ++descriptor_count;
    }
  }
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptors_allocated[3];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(descriptor_count,
                                                             descriptors_allocated)) {
    command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
    return false;
  }
  u32 descriptor_write_index = 0;
  command_processor_.SetExternalPipeline(pipeline);
  command_list.D3DSetComputeRootSignature(load_root_signature_.Get());
  REX_ASSERT(descriptor_write_index < descriptor_count);
  gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_dest =
      descriptors_allocated[descriptor_write_index++];
  gpu::d3d12::objects::CreateBufferTypedUAV(
      device, descriptor_dest.first, copy_buffer,
      gpu::d3d12::objects::GetUintPow2DXGIFormat(load_shader_info.dest_bpe_log2),
      static_cast<u32>(copy_buffer_size) >> load_shader_info.dest_bpe_log2);
  command_list.D3DSetComputeRootDescriptorTable(2, descriptor_dest.second);
  // Scaled sources depend on the current buffer, so their descriptors are set later in the loop.
  if (!texture_resolution_scaled) {
    D3D12SharedMemory& d3d12_shared_memory = static_cast<D3D12SharedMemory&>(GetSharedMemory());
    d3d12_shared_memory.UseForReading();
    gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_unscaled_source;
    if (bindless_resources_used_) {
      descriptor_unscaled_source = command_processor_.GetSharedMemoryUintPow2BindlessSRVHandlePair(
          load_shader_info.source_bpe_log2);
    } else {
      REX_ASSERT(descriptor_write_index < descriptor_count);
      descriptor_unscaled_source = descriptors_allocated[descriptor_write_index++];
      d3d12_shared_memory.WriteUintPow2SRVDescriptor(descriptor_unscaled_source.first,
                                                     load_shader_info.source_bpe_log2);
    }
    command_list.D3DSetComputeRootDescriptorTable(1, descriptor_unscaled_source.second);
  }

  LoadConstants load_constants;
  // 3 bits for each.
  REX_ASSERT(texture_resolution_scale_x <= 7);
  REX_ASSERT(texture_resolution_scale_y <= 7);
  load_constants.is_tiled_3d_endian_scale =
      static_cast<u32>(texture_key.tiled) | (static_cast<u32>(is_3d_tiling) << 1) |
      (static_cast<u32>(texture_key.endianness) << 2) | (texture_resolution_scale_x << 4) |
      (texture_resolution_scale_y << 7);

  // Base and mips can need different scaled resolve ranges, and descriptors need the buffer start.
  bool scaled_mips_source_set_up = false;
  u32 guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (u32 loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    u32 level = (level_packed == 0) ? 0 : loop_level;

    u32 guest_address = (is_base ? texture_key.base_page : texture_key.mip_page) << 12;

    if (texture_resolution_scaled && (is_base || !scaled_mips_source_set_up)) {
      u32 guest_size_unscaled =
          is_base ? d3d12_texture.GetGuestBaseSize() : d3d12_texture.GetGuestMipsSize();
      if (!UpdateScaledResolveRange(guest_address, guest_size_unscaled,
                                    load_shader_info.source_bpe_log2)) {
        command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);
        return false;
      }
      TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      REX_ASSERT(descriptor_write_index < descriptor_count);
      gpu::d3d12::objects::DescriptorCPUGPUHandlePair descriptor_scaled_source =
          descriptors_allocated[descriptor_write_index++];
      CreateCurrentScaledResolveRangeUintPow2SRV(descriptor_scaled_source.first,
                                                 load_shader_info.source_bpe_log2);
      command_list.D3DSetComputeRootDescriptorTable(1, descriptor_scaled_source.second);
      if (!is_base) {
        scaled_mips_source_set_up = true;
      }
    }

    if (texture_resolution_scaled) {
      // The offset is applied in the buffer, as some hardware cannot address over 512 MB as R32.
      load_constants.guest_offset = 0;
    } else {
      load_constants.guest_offset = guest_address;
    }
    if (!is_base) {
      load_constants.guest_offset += guest_layout.mip_offsets_bytes[level] *
                                     (texture_resolution_scale_x * texture_resolution_scale_y);
    }
    const xenos::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    u32 level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (texture_key.tiled) {
      // Shaders expect pitch in blocks for tiled textures.
      level_guest_pitch /= bytes_per_block;
      REX_ASSERT((level_guest_pitch & (xenos::kTextureTileWidthHeight - 1)) == 0);
    }
    load_constants.guest_pitch_aligned = level_guest_pitch;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_layer_stride_block_rows;
    REX_ASSERT(!is_3d_tiling || !(load_constants.guest_z_stride_block_rows_aligned &
                                  (xenos::kTextureTileWidthHeight - 1)));

    u32 level_width, level_height, level_depth;
    if (level == level_packed) {
      // The packed tail holds other levels too, so load the entire extents.
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, static_cast<u32>(1));
      level_height = std::max(height >> level, static_cast<u32>(1));
      level_depth = std::max(depth >> level, static_cast<u32>(1));
    }
    load_constants.size_blocks[0] =
        (level_width + (block_width - 1)) / block_width * texture_resolution_scale_x;
    load_constants.size_blocks[1] =
        (level_height + (block_height - 1)) / block_height * texture_resolution_scale_y;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;

    u32 group_count_x =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    u32 group_count_y =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;

    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& level_host_layer_layout =
        is_base ? host_layer_layout_base : host_layer_layouts_mips[level];
    u32 host_layer_size =
        static_cast<u32>(is_base ? host_layer_size_base : host_layer_sizes_mips[level]);
    load_constants.host_offset = static_cast<u32>(level_host_layer_layout.Offset);
    load_constants.host_pitch = level_host_layer_layout.Footprint.RowPitch;

    command_list.D3DSetComputeRoot32BitConstants(0, sizeof(load_constants) / sizeof(u32),
                                                 &load_constants, 0);

    u32 level_array_layer_stride_bytes_scaled =
        level_guest_layout.array_layer_stride_bytes *
        (texture_resolution_scale_x * texture_resolution_scale_y);
    for (u32 layer = 0; layer < array_size; ++layer) {
      if (layer != 0) {
        command_list.D3DSetComputeRoot32BitConstants(
            0, 1, &load_constants.guest_offset,
            offsetof(LoadConstants, guest_offset) / sizeof(u32));
        command_list.D3DSetComputeRoot32BitConstants(
            0, 1, &load_constants.host_offset, offsetof(LoadConstants, host_offset) / sizeof(u32));
      }
      REX_ASSERT(copy_buffer_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      command_processor_.SubmitBarriers();
      command_list.D3DDispatch(group_count_x, group_count_y, load_constants.size_blocks[2]);
      load_constants.guest_offset += level_array_layer_stride_bytes_scaled;
      load_constants.host_offset += host_layer_size;
    }
  }

  d3d12_texture.MarkAsUsed();

  ID3D12Resource* texture_resource = d3d12_texture.GetResource();
  command_processor_.PushTransitionBarrier(
      texture_resource, d3d12_texture.SetResourceState(D3D12_RESOURCE_STATE_COPY_DEST),
      D3D12_RESOURCE_STATE_COPY_DEST);
  command_processor_.PushTransitionBarrier(copy_buffer, copy_buffer_state,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
  copy_buffer_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
  command_processor_.SubmitBarriers();
  u32 texture_level_count = texture_key.mip_max_level + 1;
  D3D12_TEXTURE_COPY_LOCATION location_source, location_dest;
  location_source.pResource = copy_buffer;
  location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  location_dest.pResource = texture_resource;
  location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  for (u32 level = level_first; level <= level_last; ++level) {
    u32 guest_level = std::min(level, level_packed);
    location_source.PlacedFootprint =
        level ? host_layer_layouts_mips[guest_level] : host_layer_layout_base;
    location_dest.SubresourceIndex = level;
    UINT64 host_layer_size = level ? host_layer_sizes_mips[guest_level] : host_layer_size_base;
    D3D12_BOX source_box;
    const D3D12_BOX* source_box_ptr;
    if (level >= level_packed) {
      u32 level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      xenos::GetPackedMipOffset(width, height, depth, guest_format, level, level_offset_blocks_x,
                                level_offset_blocks_y, level_offset_z);
      source_box.left = level_offset_blocks_x * block_width * texture_resolution_scale_x;
      source_box.top = level_offset_blocks_y * block_height * texture_resolution_scale_y;
      source_box.front = level_offset_z;
      source_box.right =
          source_box.left +
          rex::Align(std::max((width * texture_resolution_scale_x) >> level, static_cast<u32>(1)),
                     host_block_width);
      source_box.bottom =
          source_box.top +
          rex::Align(std::max((height * texture_resolution_scale_y) >> level, static_cast<u32>(1)),
                     host_block_height);
      source_box.back = source_box.front + std::max(depth >> level, static_cast<u32>(1));
      source_box_ptr = &source_box;
    } else {
      source_box_ptr = nullptr;
    }
    for (u32 layer = 0; layer < array_size; ++layer) {
      command_list.D3DCopyTextureRegion(&location_dest, 0, 0, 0, &location_source, source_box_ptr);
      location_dest.SubresourceIndex += texture_level_count;
      location_source.PlacedFootprint.Offset += host_layer_size;
    }
  }

  command_processor_.ReleaseScratchGPUBuffer(copy_buffer, copy_buffer_state);

  return true;
}

void D3D12TextureCache::UpdateTextureBindingsImpl(u32 fetch_constant_mask) {
  u32 bindings_remaining = fetch_constant_mask;
  u32 binding_index;
  while (rex::bit_scan_forward(bindings_remaining, &binding_index)) {
    bindings_remaining &= ~(UINT32_C(1) << binding_index);
    D3D12TextureBinding& d3d12_binding = d3d12_texture_bindings_[binding_index];
    d3d12_binding.Reset();
    const TextureBinding* binding = GetValidTextureBinding(binding_index);
    if (!binding) {
      continue;
    }
    if (IsSignedVersionSeparateForFormat(binding->key)) {
      if (binding->texture && xenos::IsAnySignNotSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture),
                                          binding->key.dimension, false, binding->host_swizzle);
      }
      if (binding->texture_signed && xenos::IsAnySignSigned(binding->swizzled_signs)) {
        d3d12_binding.descriptor_index_signed =
            FindOrCreateTextureDescriptor(*static_cast<D3D12Texture*>(binding->texture_signed),
                                          binding->key.dimension, true, binding->host_swizzle);
      }
    } else {
      D3D12Texture* texture = static_cast<D3D12Texture*>(binding->texture);
      if (texture) {
        if (xenos::IsAnySignNotSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, false, binding->host_swizzle);
        }
        if (xenos::IsAnySignSigned(binding->swizzled_signs)) {
          d3d12_binding.descriptor_index_signed = FindOrCreateTextureDescriptor(
              *texture, binding->key.dimension, true, binding->host_swizzle);
        }
      }
    }
  }
}

ID3D12Resource* D3D12TextureCache::D3D12Texture::GetOrCreate3DAs2DResource(
    D3D12_RESOURCE_STATES end_state) {
  if (!REXCVAR_GET(::rex::gpu::cvars::texture_3d_to_2d)) {
    return nullptr;
  }

  auto& d3d12_cache = static_cast<D3D12TextureCache&>(GetTextureCache());

  if (texture_3d_as_2d_) {
    d3d12_cache.command_processor_.PushTransitionBarrier(
        texture_3d_as_2d_->GetResource(), texture_3d_as_2d_->SetResourceState(end_state),
        end_state);
    return texture_3d_as_2d_->GetResource();
  }

  const gpu::d3d12::D3D12Provider& provider = d3d12_cache.command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = GetKey().GetWidth();
  desc.Height = GetKey().GetHeight();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = d3d12_cache.GetDXGIResourceFormat(GetKey());
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    return nullptr;
  }
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COPY_DEST;
  Microsoft::WRL::ComPtr<ID3D12Resource> resource_2d;
  if (FAILED(device->CreateCommittedResource(&gpu::d3d12::objects::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &desc,
                                             initial_state, nullptr, IID_PPV_ARGS(&resource_2d)))) {
    REX_FAIL(gpu, nullptr, "D3D12TextureCache: Failed to create 3D-as-2D wrapper resource");
  }

  TextureKey key_2d = GetKey();
  key_2d.depth_or_array_size_minus_1 = 0;
  key_2d.mip_max_level = 0;
  texture_3d_as_2d_ =
      std::make_unique<D3D12Texture>(d3d12_cache, key_2d, resource_2d.Get(), initial_state, false);
  texture_3d_as_2d_->SetForceLoad3DTiling(true);

  if (!d3d12_cache.LoadTextureData(*texture_3d_as_2d_)) {
    REXLOG_ERROR(gpu, "D3D12TextureCache: Failed to load 3D-as-2D wrapper data");
    texture_3d_as_2d_.reset();
    return nullptr;
  }

  d3d12_cache.command_processor_.PushTransitionBarrier(
      texture_3d_as_2d_->GetResource(), texture_3d_as_2d_->SetResourceState(end_state), end_state);
  return texture_3d_as_2d_->GetResource();
}

u32 D3D12TextureCache::FindOrCreateTextureDescriptor(D3D12Texture& texture,
                                                     xenos::DataDimension dimension, bool is_signed,
                                                     u32 host_swizzle) {
  D3D12Texture::SRVDescriptorKey descriptor_key;
  descriptor_key.key = 0;
  descriptor_key.is_signed = static_cast<u32>(is_signed);
  descriptor_key.host_swizzle = host_swizzle;
  descriptor_key.dimension = static_cast<u32>(dimension);

  u32 existing_descriptor_index = texture.GetSRVDescriptorIndex(descriptor_key);
  if (existing_descriptor_index != UINT32_MAX) {
    REX_PERF_INC(kTextureCacheHits);
    return existing_descriptor_index;
  }
  REX_PERF_INC(kTextureCacheMisses);

  TextureKey texture_key = texture.GetKey();

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};

  if (IsSignedVersionSeparateForFormat(texture_key) &&
      texture_key.signed_separate != static_cast<u32>(is_signed)) {
    return UINT32_MAX;
  }
  xenos::TextureFormat format = texture_key.format;
  if (is_signed) {
    // Signed compressed textures (DXN and DXT5A) are not supported.
    desc.Format = kHostFormats[static_cast<u32>(format)].dxgi_format_signed;
  } else {
    desc.Format = GetDXGIUnormFormat(texture_key);
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN) {
    unsupported_format_features_used_[static_cast<u32>(format)] |=
        is_signed ? kUnsupportedSnormBit : kUnsupportedUnormBit;
    return UINT32_MAX;
  }

  u32 mip_levels = texture_key.mip_max_level + 1;
  ID3D12Resource* resource_for_view = texture.GetResource();
  switch (dimension) {
    case xenos::DataDimension::k3D:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      desc.Texture3D.MostDetailedMip = 0;
      desc.Texture3D.MipLevels = mip_levels;
      desc.Texture3D.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::kCube:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      desc.TextureCube.MostDetailedMip = 0;
      desc.TextureCube.MipLevels = mip_levels;
      desc.TextureCube.ResourceMinLODClamp = 0.0f;
      break;
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      if (texture_key.dimension == xenos::DataDimension::k3D) {
        resource_for_view =
            texture.GetOrCreate3DAs2DResource(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!resource_for_view) {
          return UINT32_MAX;
        }
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = 1;
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      } else {
        desc.Texture2DArray.MostDetailedMip = 0;
        desc.Texture2DArray.MipLevels = mip_levels;
        desc.Texture2DArray.FirstArraySlice = 0;
        desc.Texture2DArray.ArraySize = texture_key.GetDepthOrArraySize();
        desc.Texture2DArray.PlaneSlice = 0;
        desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
      }
      break;
    default:
      REX_UNHANDLED_CASE(dimension);
      return UINT32_MAX;
  }

  desc.Shader4ComponentMapping =
      host_swizzle | D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;

  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  u32 descriptor_index;
  if (bindless_resources_used_) {
    descriptor_index = command_processor_.RequestPersistentViewBindlessDescriptor();
    if (descriptor_index == UINT32_MAX) {
      REX_FAIL(gpu, UINT32_MAX,
               "Failed to create a texture descriptor - no free bindless view "
               "descriptors");
    }
  } else {
    if (srv_descriptor_cache_free_count_) {
      descriptor_index = srv_descriptor_cache_free_[--srv_descriptor_cache_free_count_];
    } else {
      // Allocated + 1 including the new descriptor, rounded up to the page size.
      u32 cache_pages_needed = (srv_descriptor_cache_allocated_ + kSRVDescriptorCachePageSize) /
                               kSRVDescriptorCachePageSize;
      if (srv_descriptor_cache_.size() < cache_pages_needed) {
        D3D12_DESCRIPTOR_HEAP_DESC cache_heap_desc;
        cache_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cache_heap_desc.NumDescriptors = kSRVDescriptorCachePageSize;
        cache_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        cache_heap_desc.NodeMask = 0;
        srv_descriptor_cache_.reserve(cache_pages_needed);
        srv_descriptor_cache_free_.resize(size_t{cache_pages_needed} * kSRVDescriptorCachePageSize);
        while (srv_descriptor_cache_.size() < cache_pages_needed) {
          Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cache_heap;
          if (FAILED(device->CreateDescriptorHeap(&cache_heap_desc, IID_PPV_ARGS(&cache_heap)))) {
            REX_FAIL(gpu, UINT32_MAX,
                     "D3D12TextureCache: Failed to create a texture descriptor - "
                     "couldn't create a descriptor cache heap");
          }
          srv_descriptor_cache_.emplace_back(cache_heap.Get());
        }
      }
      descriptor_index = srv_descriptor_cache_allocated_++;
    }
  }
  device->CreateShaderResourceView(resource_for_view, &desc,
                                   GetTextureDescriptorCPUHandle(descriptor_index));
  texture.AddSRVDescriptorIndex(descriptor_key, descriptor_index);
  return descriptor_index;
}

void D3D12TextureCache::ReleaseTextureDescriptor(u32 descriptor_index) noexcept {
  if (bindless_resources_used_) {
    command_processor_.ReleaseViewBindlessDescriptorImmediately(descriptor_index);
  } else {
    srv_descriptor_cache_free_[srv_descriptor_cache_free_count_++] = descriptor_index;
  }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12TextureCache::GetTextureDescriptorCPUHandle(
    u32 descriptor_index) const {
  const gpu::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  if (bindless_resources_used_) {
    return provider.OffsetViewDescriptor(command_processor_.GetViewBindlessHeapCPUStart(),
                                         descriptor_index);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE heap_start =
      srv_descriptor_cache_[descriptor_index / kSRVDescriptorCachePageSize].GetHeapStart();
  u32 heap_offset = descriptor_index % kSRVDescriptorCachePageSize;
  return provider.OffsetViewDescriptor(heap_start, heap_offset);
}

xenos::ClampMode D3D12TextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No Direct3D 12 equivalent of GL_CLAMP (clamp to half edge, half border).
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    // No Direct3D 12 equivalents.
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

}  // namespace rex::gpu::d3d12
