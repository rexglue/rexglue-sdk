/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_SHARED_TEXTURE_FORMAT_TABLE_H_
#define XENIA_GPU_METAL_METAL_SHARED_TEXTURE_FORMAT_TABLE_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/xenos/state.h>

namespace rex {
namespace graphics {
namespace metal {

// Per-guest-format routing entry for the shared-buffer GPU texture load route
// (optimize-metal-gpu-resident-resolve-coherence, stage 1). One row per
// xenos::TextureFormat the route serves; anything not listed stays on the
// legacy CPU cache.
//
// Host formats, swizzles and signed views mirror the legacy cache's
// PlanForFormat (the byte oracle every routed class is proven against) and
// upstream D3D12's host_formats_ table. Values are MTLPixelFormat raw values
// so the header stays Objective-C free; 0 (MTLPixelFormatInvalid) = none.
// Upstream texture-load family (bit-copy families only in tranche 1); the
// shared cache maps these onto TextureCache::LoadShaderIndex, which is a
// protected member of the base class.
enum class SharedLoadFamily : uint32_t {
  k8bpb,
  k16bpb,
  k32bpb,
  k64bpb,
  // 16bpp 4:4:4:4 expanded to RGBA8 (texture_load_r4g4b4a4_r8g8b8a8): the
  // host block is 4 bytes, twice the guest's. For GPUs without ABGR4Unorm.
  k16bpbRgba4ToRgba8,
  // 16bpp 5:6:5 expanded to opaque RGBA8 (texture_load_r5g6b5_r8g8b8a8), for
  // GPUs without B5G6R5Unorm. Apple GPUs keep 5:6:5 on the legacy cache.
  k16bpbR5G6B5ToRgba8,
  // 16bpp 1:5:5:5 expanded to RGBA8 (texture_load_r5g5b5a1_r8g8b8a8), for
  // GPUs without BGR5A1Unorm.
  k16bpbR5G5B5A1ToRgba8,
  // CTX1 (Xenos two-channel normal-map compression, 8-byte 4x4 blocks)
  // decoded to RG8 texels (texture_load_ctx1). The legacy cache decodes it on
  // the CPU at the base level only.
  kCtx1ToRg8,
  // 32bpp packed 11:11:10 / 10:11:11 unorm expanded to RGBA16 (the upstream
  // texture_load_r11g11b10_rgba16 / r10g11b11 shaders): the host block is 8
  // bytes, twice the guest's. Signed bindings take an RGBA16Snorm view of
  // the same bytes (the exact snorm decode is a separate shader the view
  // cannot express; the HDR chains that use these formats fetch unsigned).
  kR11G11B10ToRgba16,
  kR10G11B11ToRgba16,
  // DXT3A (the DXT3 alpha block alone, 8-byte 4x4 blocks) expanded to R8
  // texels (texture_load_dxt3a), as the legacy CPU decode does at the base
  // level only.
  kDxt3aToR8,
  // 16-byte 4x4 block copy (texture_load_128bpb) for DXT3 / DXT5 / DXN kept
  // compressed on the host (BC2 / BC3 / BC5); DXT1 / DXT5A take k64bpb
  // (BC1 / BC4). 2026-10-16: Rage's 16 MB linear DXT5 megatexture page cache
  // was a CPU untile every frame.
  k128bpb,
  // 32-bit depth texels (k_24_8 unorm24 / k_24_8_FLOAT 20e4) converted to
  // R32Float on the GPU (texture_load_depth_unorm / _float, the same rule the
  // legacy CPU decode implements). 2026-10-16: Rage's 768x768 24_8_FLOAT
  // shadow maps were 5 ms/frame of CPU decode.
  kDepthUnormToR32,
  kDepthFloatToR32,
  // 6:5:5 (guest R5 G5 B6) repacked into B5G6R5 with guest blue in the host
  // green field and guest green in the blue field (the upstream
  // texture_load_r5g5b6_b5g6r5_swizzle_rbga shader); the host swizzle RBGG
  // reads them back, as the D3D12 and Vulkan tables do. Apple GPUs only
  // (B5G6R5Unorm).
  kR5G5B6ToB5G6R5Rbga,
  // DXT3A_AS_1_1_1_1 (one bit per channel in each 4-bit group of the DXT3
  // alpha block) expanded to 16-bit texels by the upstream
  // texture_load_dxt3aas1111_argb4 shader: alpha in bits 0-3, red 4-7, green
  // 8-11, blue 12-15, which Metal's ABGR4Unorm reads as A, B, G, R, so the
  // host swizzle exchanges red and blue. Apple GPUs only. Guest 8-byte 4x4
  // blocks, 2 host bytes per texel. Resolves already produce this format
  // (Kinect titles' tracking masks, 5345084D).
  kDxt3aAs1111ToAbgr4,
  // 4:2:2 chroma-subsampled texel pairs (k_Cr_Y1_Cb_Y0_REP: the bytes Cr Y1
  // Cb Y0, D3D's G8R8_G8B8; k_Y1_Cr_Y0_Cb_REP: R8G8_B8G8) expanded to two
  // RGBA8 texels with the shared chroma replicated and blue written to alpha
  // (texture_load_gbgr8_rgb8 / texture_load_bgrg8_rgb8, the decompressing
  // alternative the D3D12 table keeps). A 2x1 guest block of 4 bytes becomes
  // 8 host bytes.
  kGbgr8ToRgba8,
  kBgrg8ToRgba8,
};

struct SharedTextureFormatEntry {
  xenos::TextureFormat format;
  // Load shader family (non-scaled); the direct RGBA8 pipeline is selected by
  // `direct` instead.
  SharedLoadFamily load_shader;
  uint32_t host_format;         // MTLPixelFormat for unsigned/float bindings.
  uint32_t host_format_signed;  // MTLPixelFormat view for signed bindings, or 0.
  uint32_t host_swizzle;        // xenos XE_GPU_TEXTURE_SWIZZLE_* composition.
  uint32_t bytes_per_block;     // Guest bytes per block (== host bytes
                                // except for the expanding families).
  bool direct;                  // texture_load_32bpb_direct (writes texture).
};

// MTLPixelFormat raw values (Metal/MTLPixelFormat.h), duplicated here so the
// table is usable from C++ translation units and tests.
enum : uint32_t {
  kMtlPixelFormatInvalid = 0,
  kMtlPixelFormatR8Unorm = 10,
  kMtlPixelFormatB5G6R5Unorm = 40,
  kMtlPixelFormatABGR4Unorm = 42,
  kMtlPixelFormatBGR5A1Unorm = 43,
  kMtlPixelFormatR8Snorm = 12,
  kMtlPixelFormatR16Unorm = 20,
  kMtlPixelFormatR16Snorm = 22,
  kMtlPixelFormatR16Float = 25,
  kMtlPixelFormatRG8Unorm = 30,
  kMtlPixelFormatRG8Snorm = 32,
  kMtlPixelFormatR32Float = 55,
  kMtlPixelFormatRG32Float = 105,
  kMtlPixelFormatRGBA32Float = 125,
  kMtlPixelFormatRG16Unorm = 60,
  kMtlPixelFormatRG16Snorm = 62,
  kMtlPixelFormatRG16Float = 65,
  kMtlPixelFormatRGBA8Unorm = 70,
  kMtlPixelFormatRGBA8Snorm = 72,
  kMtlPixelFormatRGB10A2Unorm = 90,
  kMtlPixelFormatRGBA16Unorm = 110,
  kMtlPixelFormatRGBA16Snorm = 112,
  kMtlPixelFormatRGBA16Float = 115,
  kMtlPixelFormatBC1_RGBA = 130,
  kMtlPixelFormatBC2_RGBA = 132,
  kMtlPixelFormatBC3_RGBA = 134,
  kMtlPixelFormatBC4_RUnorm = 140,
  kMtlPixelFormatBC4_RSnorm = 141,
  kMtlPixelFormatBC5_RGUnorm = 142,
  kMtlPixelFormatBC5_RGSnorm = 143,
};

inline const SharedTextureFormatEntry* SharedTextureFormatTable(
    size_t* count_out) {
  static const SharedTextureFormatEntry kTable[] = {
      // The originally promoted class (C1T-R): direct pipeline.
      {xenos::TextureFormat::k_8_8_8_8, SharedLoadFamily::k32bpb,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Snorm,
       xenos::REXGPU_SWIZZLE_RGBA, 4, true},
      {xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16,
       SharedLoadFamily::k32bpb, kMtlPixelFormatRGBA8Unorm,
       kMtlPixelFormatRGBA8Snorm, xenos::REXGPU_SWIZZLE_RGBA, 4, true},
      // Tranche 1: bit-copy families (endian swap only), the resolve-producible
      // formats the untile timer named on JC2 / Sonic / DOA5.
      {xenos::TextureFormat::k_8, SharedLoadFamily::k8bpb,
       kMtlPixelFormatR8Unorm, kMtlPixelFormatR8Snorm,
       xenos::REXGPU_SWIZZLE_RRRR, 1, false},
      {xenos::TextureFormat::k_8_8, SharedLoadFamily::k16bpb,
       kMtlPixelFormatRG8Unorm, kMtlPixelFormatRG8Snorm,
       xenos::REXGPU_SWIZZLE_RGGG, 2, false},
      {xenos::TextureFormat::k_16, SharedLoadFamily::k16bpb,
       kMtlPixelFormatR16Unorm, kMtlPixelFormatR16Snorm,
       xenos::REXGPU_SWIZZLE_RRRR, 2, false},
      {xenos::TextureFormat::k_16_16, SharedLoadFamily::k32bpb,
       kMtlPixelFormatRG16Unorm, kMtlPixelFormatRG16Snorm,
       xenos::REXGPU_SWIZZLE_RGGG, 4, false},
      {xenos::TextureFormat::k_16_16_16_16, SharedLoadFamily::k64bpb,
       kMtlPixelFormatRGBA16Unorm, kMtlPixelFormatRGBA16Snorm,
       xenos::REXGPU_SWIZZLE_RGBA, 8, false},
      {xenos::TextureFormat::k_16_FLOAT, SharedLoadFamily::k16bpb,
       kMtlPixelFormatR16Float, kMtlPixelFormatR16Float,
       xenos::REXGPU_SWIZZLE_RRRR, 2, false},
      {xenos::TextureFormat::k_16_EXPAND, SharedLoadFamily::k16bpb,
       kMtlPixelFormatR16Float, kMtlPixelFormatR16Float,
       xenos::REXGPU_SWIZZLE_RRRR, 2, false},
      {xenos::TextureFormat::k_16_16_FLOAT, SharedLoadFamily::k32bpb,
       kMtlPixelFormatRG16Float, kMtlPixelFormatRG16Float,
       xenos::REXGPU_SWIZZLE_RGGG, 4, false},
      {xenos::TextureFormat::k_16_16_EXPAND,
       SharedLoadFamily::k32bpb, kMtlPixelFormatRG16Float,
       kMtlPixelFormatRG16Float, xenos::REXGPU_SWIZZLE_RGGG, 4, false},
      {xenos::TextureFormat::k_16_16_16_16_FLOAT,
       SharedLoadFamily::k64bpb, kMtlPixelFormatRGBA16Float,
       kMtlPixelFormatRGBA16Float, xenos::REXGPU_SWIZZLE_RGBA, 8,
       false},
      {xenos::TextureFormat::k_16_16_16_16_EXPAND,
       SharedLoadFamily::k64bpb, kMtlPixelFormatRGBA16Float,
       kMtlPixelFormatRGBA16Float, xenos::REXGPU_SWIZZLE_RGBA, 8,
       false},
      {xenos::TextureFormat::k_32_FLOAT, SharedLoadFamily::k32bpb,
       kMtlPixelFormatR32Float, kMtlPixelFormatR32Float,
       xenos::REXGPU_SWIZZLE_RRRR, 4, false},
      // Two-channel 32-bit float (Tomb Raider's 1x1 luminance readback; off
      // the route its draw was a RAM consumer of the resolve: commit + wait).
      {xenos::TextureFormat::k_32_32_FLOAT, SharedLoadFamily::k64bpb,
       kMtlPixelFormatRG32Float, kMtlPixelFormatRG32Float,
       xenos::REXGPU_SWIZZLE_RGGG, 8, false},
      // Same bit layout as RGB10A2; no snorm sibling in Metal, a signed fetch
      // keeps the unorm view (legacy plan does the same).
      {xenos::TextureFormat::k_2_10_10_10, SharedLoadFamily::k32bpb,
       kMtlPixelFormatRGB10A2Unorm, kMtlPixelFormatRGB10A2Unorm,
       xenos::REXGPU_SWIZZLE_RGBA, 4, false},
      {xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16,
       SharedLoadFamily::k32bpb, kMtlPixelFormatRGB10A2Unorm,
       kMtlPixelFormatRGB10A2Unorm, xenos::REXGPU_SWIZZLE_RGBA, 4,
       false},
      // 4:4:4:4 (2026-10-04, Red Dead Redemption: 49 resolve-backed CPU
      // decodes per frame behind 26 blocking consumer waits at its title
      // scene). After the fetch-endian swap the guest word holds R in bits
      // 0-3 through A in bits 12-15 (the legacy decode_4444 oracle); Metal's
      // ABGR4Unorm reads A from bits 0-3 through R from bits 12-15 (device
      // probe), so the host swizzle reverses the channels. No snorm sibling.
      // XE_METAL_TEX_ROUTE_4444=0 drops this entry (legacy CPU decode).
      {xenos::TextureFormat::k_4_4_4_4, SharedLoadFamily::k16bpb,
       kMtlPixelFormatABGR4Unorm, kMtlPixelFormatABGR4Unorm,
       REXGPU_SWIZZLE(A, B, G, R), 2, false},
      // The same texels expanded to RGBA8 on the GPU where ABGR4Unorm does
      // not exist (Intel/AMD): without it Red Dead's ~34 resolve-backed
      // 4:4:4:4 fetches a frame fell back to blocking CPU decodes (Mac Pro
      // cinematic 2.7 fps, 2026-10-10). Selected only without packed16.
      {xenos::TextureFormat::k_4_4_4_4, SharedLoadFamily::k16bpbRgba4ToRgba8,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Unorm,
       // The shader's nibble order (R = bits 4-7, G = 0-3, B = 12-15,
       // A = 8-11 of the swapped word) to guest X/Y/Z/W = bits 0-3 ... 12-15,
       // the order the ABGR4 entry and the legacy decode use.
       REXGPU_SWIZZLE(G, R, A, B), 2, false},
      // 5:6:5 expanded to RGBA8 on the GPU where B5G6R5Unorm does not exist
      // (Intel/AMD). The legacy CPU decode served it at the base level only
      // (Skate 2 on the Mac Pro). The shader writes guest R/G/B to host R/G/B
      // with alpha 1; the shared table reads alpha from blue (RGBB), as the
      // legacy decode does. Selected only without packed16.
      // XE_METAL_TEX_ROUTE_565=0 drops this entry (legacy CPU decode).
      {xenos::TextureFormat::k_5_6_5, SharedLoadFamily::k16bpbR5G6B5ToRgba8,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Unorm,
       REXGPU_SWIZZLE(R, G, B, B), 2, false},
      // 5:6:5 native (Apple, packed16), 2026-10-16: the raw 16-bit copy into
      // B5G6R5Unorm with the legacy plan's host swizzle (guest R sits in the
      // low bits, Metal's B field; alpha from the high field, as the legacy
      // CPU path reads it), so routed and legacy serve the same bits. Until
      // now Apple GPUs kept 5:6:5 on the CPU (Rage: two linear mip chains,
      // 6.7 ms/frame). Selected only with packed16; XE_METAL_TEX_ROUTE_565=0
      // drops it.
      {xenos::TextureFormat::k_5_6_5, SharedLoadFamily::k16bpb,
       kMtlPixelFormatB5G6R5Unorm, kMtlPixelFormatB5G6R5Unorm,
       REXGPU_SWIZZLE(B, G, R, R), 2, false},
      // 1:5:5:5 native (Apple, packed16), 2026-10-19: the raw 16-bit copy into
      // BGR5A1Unorm with the legacy plan's host swizzle (guest R in the low
      // bits, Metal's B field; device probe), so routed and legacy serve the
      // same bits. Fable II's 256x256 mipped 1:5:5:5 textures were neither
      // served nor routable: their draws waited on the GPU 21 times a frame.
      // Selected only with packed16; XE_METAL_TEX_1555=0 drops it.
      {xenos::TextureFormat::k_1_5_5_5, SharedLoadFamily::k16bpb,
       kMtlPixelFormatBGR5A1Unorm, kMtlPixelFormatBGR5A1Unorm,
       REXGPU_SWIZZLE(B, G, R, A), 2, false},
      // The same texels expanded to RGBA8 on the GPU where BGR5A1Unorm does
      // not exist (Intel/AMD), in guest channel order, so the host swizzle is
      // the identity. Without it the format was unserved there and Fable II
      // ran 20.7 fps on the Radeon Pro 580X, as the M1 Max did before
      // BGR5A1Unorm (2026-11-06). Selected only without packed16.
      {xenos::TextureFormat::k_1_5_5_5, SharedLoadFamily::k16bpbR5G5B5A1ToRgba8,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Unorm,
       xenos::REXGPU_SWIZZLE_RGBA, 2, false},
      // CTX1 decoded to RG8 on the GPU, mips included (Halo 3 materials: 134
      // mipped uploads in a 40 s sweep served base-only by the legacy CPU
      // decode, on every GPU). RGGG like the legacy plan; no snorm sibling,
      // as in the legacy plan. XE_METAL_TEX_ROUTE_CTX1=0 drops this entry.
      {xenos::TextureFormat::k_CTX1, SharedLoadFamily::kCtx1ToRg8,
       kMtlPixelFormatRG8Unorm, kMtlPixelFormatRG8Unorm,
       xenos::REXGPU_SWIZZLE_RGGG, 8, false},
      // Packed 11:11:10 / 10:11:11 (resolve-backed HDR chains: Tomb Raider's
      // 320x180 / 80x48 / 20x12 downsamples) expanded to RGBA16 on the GPU.
      // Off the route these are RAM consumers of the resolve, and every draw
      // sampling one paid a commit + GPU wait (58 drains, 25 ms a frame).
      // XE_METAL_TEX_ROUTE_1110=0 drops these entries.
      // The Xenos names count fields from the most significant bit, the load
      // shaders from the least: k_11_11_10 keeps red in the LOW 10 bits, so
      // it is the r10g11b11 loader (and the resolve packs it as 10:11:11),
      // exactly as the D3D12 and Vulkan tables pair them. The reverse pairing
      // decoded Rise of the Tomb Raider's bloom chain into contour noise.
      {xenos::TextureFormat::k_11_11_10,
       SharedLoadFamily::kR10G11B11ToRgba16, kMtlPixelFormatRGBA16Unorm,
       kMtlPixelFormatRGBA16Snorm, xenos::REXGPU_SWIZZLE_RGBA, 4,
       false},
      {xenos::TextureFormat::k_10_11_11,
       SharedLoadFamily::kR11G11B10ToRgba16, kMtlPixelFormatRGBA16Unorm,
       kMtlPixelFormatRGBA16Snorm, xenos::REXGPU_SWIZZLE_RGBA, 4,
       false},
      // DXT3A expanded to R8 on the GPU, mips included (Halo 3, Viva Pinata).
      // RRRR like the legacy plan; no snorm sibling.
      // XE_METAL_TEX_ROUTE_DXT3A=0 drops this entry.
      {xenos::TextureFormat::k_DXT3A, SharedLoadFamily::kDxt3aToR8,
       kMtlPixelFormatR8Unorm, kMtlPixelFormatR8Unorm,
       xenos::REXGPU_SWIZZLE_RRRR, 8, false},
      // Block-compressed formats kept compressed on the host (BC1-BC5, native
      // on Apple GPUs; the legacy plan uploads the same blocks after a CPU
      // untile). 8- or 16-byte block copies with the 64bpb / 128bpb loaders;
      // host swizzles as the legacy plan (DXT5A RRRR, DXN RGGG with a snorm
      // sibling). Selected only where the device reports BC support;
      // XE_METAL_TEX_ROUTE_BC=0 drops them. 2026-10-16, Rage.
      {xenos::TextureFormat::k_DXT1, SharedLoadFamily::k64bpb,
       kMtlPixelFormatBC1_RGBA, kMtlPixelFormatBC1_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 8, false},
      {xenos::TextureFormat::k_DXT1_AS_16_16_16_16, SharedLoadFamily::k64bpb,
       kMtlPixelFormatBC1_RGBA, kMtlPixelFormatBC1_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 8, false},
      {xenos::TextureFormat::k_DXT2_3, SharedLoadFamily::k128bpb,
       kMtlPixelFormatBC2_RGBA, kMtlPixelFormatBC2_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 16, false},
      {xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16, SharedLoadFamily::k128bpb,
       kMtlPixelFormatBC2_RGBA, kMtlPixelFormatBC2_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 16, false},
      {xenos::TextureFormat::k_DXT4_5, SharedLoadFamily::k128bpb,
       kMtlPixelFormatBC3_RGBA, kMtlPixelFormatBC3_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 16, false},
      {xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16, SharedLoadFamily::k128bpb,
       kMtlPixelFormatBC3_RGBA, kMtlPixelFormatBC3_RGBA,
       xenos::REXGPU_SWIZZLE_RGBA, 16, false},
      {xenos::TextureFormat::k_DXT5A, SharedLoadFamily::k64bpb,
       kMtlPixelFormatBC4_RUnorm, kMtlPixelFormatBC4_RUnorm,
       xenos::REXGPU_SWIZZLE_RRRR, 8, false},
      {xenos::TextureFormat::k_DXN, SharedLoadFamily::k128bpb,
       kMtlPixelFormatBC5_RGUnorm, kMtlPixelFormatBC5_RGSnorm,
       xenos::REXGPU_SWIZZLE_RGGG, 16, false},
      // Depth texels to R32Float on the GPU (RRRR like the legacy plan).
      // XE_METAL_TEX_ROUTE_DEPTH=0 drops both.
      {xenos::TextureFormat::k_24_8, SharedLoadFamily::kDepthUnormToR32,
       kMtlPixelFormatR32Float, kMtlPixelFormatR32Float,
       xenos::REXGPU_SWIZZLE_RRRR, 4, false},
      {xenos::TextureFormat::k_24_8_FLOAT, SharedLoadFamily::kDepthFloatToR32,
       kMtlPixelFormatR32Float, kMtlPixelFormatR32Float,
       xenos::REXGPU_SWIZZLE_RRRR, 4, false},
      // ---- 2026-10-26 D3D12 host-format parity (texture-matrix lane) ----
      // Four-channel 32-bit float: 16-byte block copy (Gears of War 3's
      // 4096x1 UE3 float tables, served by the legacy plan since 2026-09-18;
      // the routed load keeps them off the CPU untile).
      {xenos::TextureFormat::k_32_32_32_32_FLOAT, SharedLoadFamily::k128bpb,
       kMtlPixelFormatRGBA32Float, kMtlPixelFormatRGBA32Float,
       xenos::REXGPU_SWIZZLE_RGBA, 16, false},
      // 6:5:5 repacked into B5G6R5 by the upstream loader (see the family);
      // the D3D12 table's RBGG swizzle reads guest green from the host blue
      // field and guest blue from host green, alpha from the last stored
      // component. Apple GPUs only (packed16); XE_METAL_TEX_655=0 drops it.
      {xenos::TextureFormat::k_6_5_5, SharedLoadFamily::kR5G5B6ToB5G6R5Rbga,
       kMtlPixelFormatB5G6R5Unorm, kMtlPixelFormatB5G6R5Unorm,
       REXGPU_SWIZZLE(R, B, G, G), 2, false},
      // DXT3A_AS_1_1_1_1 expanded to ABGR4 (see the family): the loader's
      // nibble order puts guest red where Metal reads blue and vice versa, so
      // the host swizzle is BGRA, as for the 1:5:5:5 entry. Apple GPUs only;
      // XE_METAL_TEX_DXT3A1111=0 drops it.
      {xenos::TextureFormat::k_DXT3A_AS_1_1_1_1,
       SharedLoadFamily::kDxt3aAs1111ToAbgr4, kMtlPixelFormatABGR4Unorm,
       kMtlPixelFormatABGR4Unorm, REXGPU_SWIZZLE(B, G, R, A), 8,
       false},
      // 4:2:2 pairs expanded to RGBA8 (see the families); RGBB like the D3D12
      // table. Every GPU; XE_METAL_TEX_422=0 drops both.
      {xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP, SharedLoadFamily::kGbgr8ToRgba8,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Unorm,
       xenos::REXGPU_SWIZZLE_RGBB, 4, false},
      {xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP, SharedLoadFamily::kBgrg8ToRgba8,
       kMtlPixelFormatRGBA8Unorm, kMtlPixelFormatRGBA8Unorm,
       xenos::REXGPU_SWIZZLE_RGBB, 4, false},
  };
  if (count_out) {
    *count_out = sizeof(kTable) / sizeof(kTable[0]);
  }
  return kTable;
}

// Packed 16-bit pixel formats (ABGR4Unorm and the 5:6:5 / 1:5:5:5 family)
// exist only on Apple GPU families: on an Intel/AMD Mac a texture descriptor
// with one fails validation (Red Dead on the RX 6900 XT aborted at its first
// 4:4:4:4 texture). The shared texture cache records the device's answer at
// initialization; entries that need the formats are dropped without it.
// Host bytes per block of an entry's textures.
// For the block-decoding families (CTX1, DXT3A) this is bytes per host TEXEL,
// the unit their load shaders write and the shared LoadShaderInfo reports.
inline uint32_t SharedTextureHostBytesPerBlock(
    const SharedTextureFormatEntry& entry) {
  switch (entry.load_shader) {
    case SharedLoadFamily::k16bpbRgba4ToRgba8:
    case SharedLoadFamily::k16bpbR5G6B5ToRgba8:
    case SharedLoadFamily::k16bpbR5G5B5A1ToRgba8:
      return 4u;
    case SharedLoadFamily::kR11G11B10ToRgba16:
    case SharedLoadFamily::kR10G11B11ToRgba16:
      return 8u;
    case SharedLoadFamily::kCtx1ToRg8:
      return 2u;
    case SharedLoadFamily::kDxt3aToR8:
      return 1u;
    case SharedLoadFamily::kDxt3aAs1111ToAbgr4:
      return 2u;
    case SharedLoadFamily::kGbgr8ToRgba8:
    case SharedLoadFamily::kBgrg8ToRgba8:
      return 8u;
    default:
      return entry.bytes_per_block;
  }
}

// The 4x4 block-compressed guest formats (decoded or kept compressed).
inline bool IsSharedTextureBlockCompressedFormat(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_CTX1:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT3A_AS_1_1_1_1:
      return true;
    default:
      return false;
  }
}
// The 4:2:2 formats: a guest block is a horizontal PAIR of texels (4 bytes).
inline bool IsSharedTexture422Format(xenos::TextureFormat format) {
  return format == xenos::TextureFormat::k_Cr_Y1_Cb_Y0_REP ||
         format == xenos::TextureFormat::k_Y1_Cr_Y0_Cb_REP;
}

// Guest block edge in texels: 4 for the 4x4 block-compressed formats, 1 for
// everything else.
inline uint32_t SharedTextureBlockDim(const SharedTextureFormatEntry& entry) {
  return IsSharedTextureBlockCompressedFormat(entry.format) ? 4u : 1u;
}
// Guest block width and height in texels: 4x4 for the block-compressed
// formats, 2x1 for the 4:2:2 pairs, 1x1 for everything else. The loaders
// dispatch over guest blocks, so both axes must use the right edge.
inline uint32_t SharedTextureBlockWidth(const SharedTextureFormatEntry& entry) {
  return IsSharedTexture422Format(entry.format) ? 2u
                                                : SharedTextureBlockDim(entry);
}
inline uint32_t SharedTextureBlockHeight(
    const SharedTextureFormatEntry& entry) {
  return SharedTextureBlockDim(entry);
}

// Host texels per guest block edge: the decoding families (CTX1, DXT3A)
// write 4x4 host texels per guest block; the BC-preserving families and
// every uncompressed format write one host block/texel per guest block.
inline uint32_t SharedTextureHostExpand(const SharedLoadFamily& family) {
  return family == SharedLoadFamily::kCtx1ToRg8 ||
                 family == SharedLoadFamily::kDxt3aToR8 ||
                 family == SharedLoadFamily::kDxt3aAs1111ToAbgr4
             ? 4u
             : 1u;
}
inline uint32_t SharedTextureHostExpand(const SharedTextureFormatEntry& entry) {
  return SharedTextureHostExpand(entry.load_shader);
}

// Whether the entry's host texture is a BC (compressed) pixel format: no
// shader-write usage, blits address block rows.
inline bool SharedTextureHostIsBc(const SharedTextureFormatEntry& entry) {
  return entry.host_format >= kMtlPixelFormatBC1_RGBA &&
         entry.host_format <= kMtlPixelFormatBC5_RGSnorm;
}

// BC texture compression support, recorded by the shared cache at
// initialization (MTLDevice supportsBCTextureCompression: every Apple
// Silicon Mac; not every Intel/AMD part). Entries with BC host formats are
// dropped without it.
inline std::atomic<bool>& SharedTextureBcSupported() {
  static std::atomic<bool> supported{false};
  return supported;
}

// Mipped 4:4:4:4 keys take the RGBA4 -> RGBA8 expansion (non-Apple GPUs):
// on three Red Dead traces it is pixel-identical to the ABGR4 route, while the
// legacy CPU decode (base level only) differs by up to 139. An earlier
// level-1 mismatch against guest memory was a bad oracle, not the shader.
// XE_METAL_RGBA4_EXPAND_MIPS=0 returns mipped keys to the legacy decode.
inline bool SharedRgba4ExpandMipsEnabled() {
  static const bool s_enabled = [] {
    const char* v = std::getenv("XE_METAL_RGBA4_EXPAND_MIPS");
    return !(v && v[0] == '0');
  }();
  return s_enabled;
}

inline std::atomic<bool>& SharedTexturePacked16Supported() {
  static std::atomic<bool> supported{true};
  return supported;
}

// XE_METAL_TEX_ROUTE_FORMATS=0 restricts the table to the originally promoted
// RGBA8 class (exact rollback of the stage-1 widening).
inline bool SharedTextureRouteFormatsWidened() {
  static const bool s_widened = [] {
    const char* v = std::getenv("XE_METAL_TEX_ROUTE_FORMATS");
    return !(v && v[0] == '0' && !v[1]);
  }();
  return s_widened;
}

inline const SharedTextureFormatEntry* FindSharedTextureFormatUncached(
    xenos::TextureFormat format) {
  // The texture KEY carries the base format (TextureCache::GetBaseFormat:
  // k_11_11_10_AS_16_16_16_16 -> k_11_11_10) while the fetch constant carries
  // the AS variant; both must resolve to the same entry, or the fetch is
  // route-eligible but the key never gets a texture (Tomb Raider's HDR
  // pyramid: UNSERVED why=no-texture, 10 RAM drains a frame).
  if (format == xenos::TextureFormat::k_11_11_10_AS_16_16_16_16) {
    format = xenos::TextureFormat::k_11_11_10;
  } else if (format == xenos::TextureFormat::k_10_11_11_AS_16_16_16_16) {
    format = xenos::TextureFormat::k_10_11_11;
  } else if (format == xenos::TextureFormat::k_8_A ||
             format == xenos::TextureFormat::k_8_B) {
    // The same base-format mapping as the shared TextureCache::GetBaseFormat
    // (the key carries k_8; D3D12 serves these through the k_8 host format).
    format = xenos::TextureFormat::k_8;
  } else if (format == xenos::TextureFormat::k_8_8_8_8_A) {
    format = xenos::TextureFormat::k_8_8_8_8;
  }
  size_t count = 0;
  const SharedTextureFormatEntry* table = SharedTextureFormatTable(&count);
  for (size_t i = 0; i < count; ++i) {
    if (table[i].format != format) {
      continue;
    }
    if (!table[i].direct && !SharedTextureRouteFormatsWidened()) {
      return nullptr;
    }
    if (table[i].format == xenos::TextureFormat::k_11_11_10 ||
        table[i].format == xenos::TextureFormat::k_10_11_11) {
      static const bool s_1110 = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_1110");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_1110) {
        return nullptr;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_4_4_4_4) {
      static const bool s_4444 = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_4444");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_4444) {
        return nullptr;
      }
      // ABGR4Unorm with packed16, the RGBA8 expansion entry without.
      const bool packed16 =
          SharedTexturePacked16Supported().load(std::memory_order_relaxed);
      const bool expands =
          table[i].load_shader == SharedLoadFamily::k16bpbRgba4ToRgba8;
      if (packed16 == expands) {
        continue;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_1_5_5_5) {
      static const bool s_1555 = [] {
        const char* v = std::getenv("XE_METAL_TEX_1555");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_1555) {
        return nullptr;
      }
      // BGR5A1Unorm with packed16, the RGBA8 expansion entry without.
      const bool packed16 =
          SharedTexturePacked16Supported().load(std::memory_order_relaxed);
      const bool native = table[i].load_shader == SharedLoadFamily::k16bpb;
      if (packed16 != native) {
        continue;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_6_5_5) {
      static const bool s_655 = [] {
        const char* v = std::getenv("XE_METAL_TEX_655");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_655 ||
          !SharedTexturePacked16Supported().load(std::memory_order_relaxed)) {
        return nullptr;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_DXT3A_AS_1_1_1_1) {
      static const bool s_dxt3a1111 = [] {
        const char* v = std::getenv("XE_METAL_TEX_DXT3A1111");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_dxt3a1111 ||
          !SharedTexturePacked16Supported().load(std::memory_order_relaxed)) {
        return nullptr;
      }
    }
    if (IsSharedTexture422Format(table[i].format)) {
      static const bool s_422 = [] {
        const char* v = std::getenv("XE_METAL_TEX_422");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_422) {
        return nullptr;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_CTX1) {
      static const bool s_ctx1 = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_CTX1");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_ctx1) {
        return nullptr;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_DXT3A) {
      static const bool s_dxt3a = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_DXT3A");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_dxt3a) {
        return nullptr;
      }
    }
    if (table[i].format == xenos::TextureFormat::k_5_6_5) {
      static const bool s_565 = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_565");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_565) {
        return nullptr;
      }
      // Two 5:6:5 entries: the native B5G6R5 one with packed16, the RGBA8
      // expansion without.
      const bool packed16 =
          SharedTexturePacked16Supported().load(std::memory_order_relaxed);
      const bool native = table[i].load_shader == SharedLoadFamily::k16bpb;
      if (packed16 != native) {
        continue;
      }
    }
    if (table[i].load_shader == SharedLoadFamily::kDepthUnormToR32 ||
        table[i].load_shader == SharedLoadFamily::kDepthFloatToR32) {
      static const bool s_depth = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_DEPTH");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_depth) {
        return nullptr;
      }
    }
    if (SharedTextureHostIsBc(table[i])) {
      static const bool s_bc = [] {
        const char* v = std::getenv("XE_METAL_TEX_ROUTE_BC");
        return !(v && v[0] == '0' && !v[1]);
      }();
      if (!s_bc ||
          !SharedTextureBcSupported().load(std::memory_order_relaxed)) {
        return nullptr;
      }
    }
    return &table[i];
  }
  return nullptr;
}

// Memoized per format and per pair of runtime support flags (packed 16-bit
// and BC host formats, the only inputs that can change after start; the
// XE_METAL_TEX_ROUTE_* switches are fixed per process). The uncached scan
// walks the whole table with its switch checks on every call, and the
// eligibility predicates call it several times per fetch of every draw:
// 1.5-3.4% of the command-processor thread across eight titles (2026-10-17
// samples). 0 = not yet looked up, 1 = no entry, otherwise the entry.
inline const SharedTextureFormatEntry* FindSharedTextureFormat(
    xenos::TextureFormat format) {
  const uint32_t index = uint32_t(format);
  if (index >= 64u) {
    return FindSharedTextureFormatUncached(format);
  }
  const uint32_t flags =
      (SharedTexturePacked16Supported().load(std::memory_order_relaxed) ? 1u
                                                                        : 0u) |
      (SharedTextureBcSupported().load(std::memory_order_relaxed) ? 2u : 0u);
  static std::atomic<uintptr_t> s_memo[4][64] = {};
  std::atomic<uintptr_t>& slot = s_memo[flags][index];
  const uintptr_t memo = slot.load(std::memory_order_relaxed);
  if (memo) {
    return memo == 1u ? nullptr
                      : reinterpret_cast<const SharedTextureFormatEntry*>(memo);
  }
  const SharedTextureFormatEntry* entry =
      FindSharedTextureFormatUncached(format);
  slot.store(entry ? reinterpret_cast<uintptr_t>(entry) : uintptr_t(1),
             std::memory_order_relaxed);
  return entry;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_SHARED_TEXTURE_FORMAT_TABLE_H_
