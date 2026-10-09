/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include <rex/xenos/register_file.h>
#include "gpu/xenos/shared_memory.h"
#include <rex/xenos/xenos.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/platform.h>

#if REX_ARCH_AMD64
// 128 bit SSSE3 (SSE2 integer comparison, pshufb from SSSE3), as AVX2 is above the minimum.
#include <simde/x86/ssse3.h>
#define REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE 16
#elif REX_ARCH_ARM64
#include <arm_neon.h>
#define REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE 16
#else
#define REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE 0
#endif  // REX_ARCH_AMD64

namespace rex::gpu {

/**
 * Normalizes primitive data for Direct3D 12 and Vulkan, down to its minimum requirements plus the
 * portability subset.
 *
 * - Triangle fans are unsupported on Direct3D 10+ and the Vulkan portability subset, so they
 *   convert to triangle lists, with and without primitive reset.
 * - Line loops are unsupported on Direct3D 12 and Vulkan, so they convert to line strips.
 * - Quads are not reproducible with line lists with adjacency without geometry shaders on some
 *   Vulkan implementations and are hard to debug in PIX due to "catastrophic failures", so they
 *   convert to triangle lists.
 * - Vulkan requires a 0xFFFF primitive restart index for 16 bit indices and 0xFFFFFFFF for 32 bit.
 *   Direct3D 12 also allows 0xFFFF for 32 bit, which is of no use as guest indices are usually big
 *   endian. The guest uses only the low 24 bits of the vertex index, so the upper 8 bits are masked
 *   out of the reset index comparison, while Vulkan and Direct3D 12 require exactly 0xFFFFFFFF.
 *   - 16 bit indices with a guest reset index other than 0xFFFF pass 0xFFFF to the host unchanged,
 *     as it is the same in either endianness. When the buffer does not use 0xFFFF as a real vertex
 *     index, the reset index is replaced with 0xFFFF in the 16 bit buffer. Otherwise the buffer
 *     converts to 32 bit and the reset index is replaced with 0xFFFFFFFF.
 *   - 32 bit indices are used unchanged when the guest reset index is 0xFFFFFF and the buffer
 *     uses only 0xFFFFFFFF for reset. Otherwise elements with (index & 0xFFFFFF) == reset_index are
 *     replaced with 0xFFFFFFFF.
 * - Some Vulkan implementations only support 24 bit indices. Titles usually pass big endian
 *   indices, so all 32 bits are needed for byte swapping, as the least significant bits are in
 *   24...31. 32 bit indices therefore load indirectly with non indexed draws that fetch the
 *   indices from the shared memory, which is not compatible with primitive restart. In that case
 *   the indices are pre swapped, masked to 24 bits and the reset index converted to 0xFFFFFFFF,
 *   giving an index buffer that can be used directly.
 *
 * SIMD paths assume alignment is not required, as AVX and Neon do not need it and an unaligned
 * instruction costs nothing when the data is aligned. Truly unaligned access may take 2 hardware
 * memory operations when a boundary of at least the vector size is crossed. To minimize unaligned
 * reads, loops process leading indices without SIMD until the source is vector aligned, then whole
 * vectors with SIMD, then the remainder without SIMD. When the source and destination were
 * co-aligned, neither loads nor stores cross cache lines. Otherwise the source is aligned, with 1
 * memory read per load, and the destination unaligned, with 1 to 2 memory writes per store. Indices
 * are at least naturally aligned to 2 or 4 bytes, as INDEX_BASE_LO in DRAW_INDEX is word aligned,
 * which host graphics APIs also require.
 */
class PrimitiveProcessor {
 public:
  enum ProcessedIndexBufferType {
    /** Auto-indexed on the host. */
    kNone,
    /**
     * GPU DMA from the shared memory. For 32 bit, indirection is needed if the host supports only
     * 24 bit indices, even without endian swapping, as the GPU ignores the upper 8 bits completely.
     */
    kGuestDMA,
    /**
     * Converted and stored in the primitive converter for the current draw command. For 32 bit
     * indices, if the host lacks full 32 bits, the buffer is always pre masked and pre swapped.
     */
    kHostConverted,
    /** Auto-indexed on the guest, with an adapter index buffer on the host. */
    kHostBuiltinForAuto,
    /** Adapter index buffer on the host for indirect loading of indices via DMA from shared memory.
     */
    kHostBuiltinForDMA,
  };

  struct ProcessingResult {
    xenos::PrimitiveType guest_primitive_type;
    xenos::PrimitiveType host_primitive_type;
    /** Includes whether tessellation is enabled (not kVertex) and the type of tessellation. */
    Shader::HostVertexShaderType host_vertex_shader_type;
    /**
     * Only used for non kVertex host_vertex_shader_type. For kAdaptive, the index buffer is always
     * from the guest, fully 32 bit, and holds the floating point tessellation factors.
     */
    xenos::TessellationMode tessellation_mode;
    uint32_t guest_draw_vertex_count;
    uint32_t host_draw_vertex_count;
    uint32_t line_loop_closing_index;
    ProcessedIndexBufferType index_buffer_type;
    uint32_t guest_index_base;
    xenos::IndexFormat host_index_format;
    xenos::Endian host_shader_index_endian;
    /**
     * The reset index, if enabled, is always 0xFFFF for host_index_format kInt16 and 0xFFFFFFFF for
     * kInt32. Never enabled for list primitive types, so safe for direct use on Vulkan.
     */
    bool host_primitive_reset_enabled;
    /**
     * Backend specific handle for the index buffer valid for the current draw, only valid for
     * index_buffer_type kHostConverted, kHostBuiltinForAuto and kHostBuiltinForDMA.
     */
    size_t host_index_buffer_handle;
    bool IsTessellated() const {
      return Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type);
    }
  };

  virtual ~PrimitiveProcessor();

  /**
   * Processes the primitives of the current draw. Submission must be open, as the index buffer in
   * the shared memory may be requested.
   *
   * @param result_out The processing result.
   * @return True when processing succeeded.
   */
  bool Process(ProcessingResult& result_out);

  /**
   * Invalidates the cache within the range.
   *
   * @param physical_address_start The start of the range.
   * @param length The length of the range.
   * @param exact_range True to invalidate only the range, false to invalidate whole buckets.
   * @return The invalidated range as start address and length.
   */
  std::pair<uint32_t, uint32_t> MemoryInvalidationCallback(uint32_t physical_address_start,
                                                           uint32_t length, bool exact_range);

 protected:
  /**
   * For host side index buffer creation, the biggest possibly needed contiguous allocation, in
   * indices.
   *
   * - No conversion needs up to 0xFFFF vertices, as the vertex count in VGT_DRAW_INITIATOR is 16
   * bit.
   * - Triangle fans to lists need 3 * (UINT16_MAX - 2), or 0x2FFF7, since every guest vertex from
   * the 3rd creates a triangle. Primitive reset only slows the amplification, as the 3 vertices
   * after a reset add 1 host vertex each, not 3 each.
   * - Line loops to strips add 1 vertex if the original primitive has at least 2 vertices, either
   *   replacing the primitive reset index with the new closing vertex or, for the final primitive,
   *   adding a vertex, so the absolute limit is UINT16_MAX + 1, or 0x10000.
   * - Quad lists to triangle lists process vertices in groups of 4, each converted to 6 vertices,
   *   so the limit is 1.5 * 0xFFFC, or 0x17FFA.
   *
   * The maximum vertex count is therefore set by triangle fan to list conversion. It also includes
   * padding for co-alignment of the source and the destination for SIMD.
   */
  static constexpr uint32_t kMinRequiredConvertedIndexBufferSize =
      sizeof(uint32_t) * (UINT16_MAX - 2) * 3 * +REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE;

  PrimitiveProcessor(const xenos::RegisterFile& register_file, runtime::Memory& memory,
                     SharedMemory& shared_memory)
      : register_file_(register_file), memory_(memory), shared_memory_(shared_memory) {}

  /**
   * Initializes the common state from the backend specific initialization function.
   *
   * Full 32 bit indices are supported when the backend supports 32 bit indices unconditionally and
   * generates no indirection logic in vertex shaders. Pass false when the host lacks full 32 bit
   * indices but supports at least 24 bit, or when the host supports 32 bit but the backend can
   * handle both cases. Without full 32 bit indices, the host must use auto indexed draws for 32 bit
   * indices of ProcessedIndexBufferType kGuestDMA, fetching the index data from the shared memory
   * buffer and endian swapping it. Indirection precludes primitive reset, so when reset is needed
   * the processor pre swaps and pre masks the index buffer to hold only host endian 0x00###### or
   * 0xFFFFFFFF values. Process then returns a kHostConverted index buffer and indirection is not
   * needed, and is impossible as the buffer is no longer in the shared memory buffer. Byte swap is
   * still needed, as 16 bit kHostConverted indices are unaffected by this. The same applies to
   * primitive type conversion of 32 bit guest indices that returns kHostConverted.
   *
   * The built in index buffer holds 32 bit entries before 16 bit ones for alignment. For primitive
   * expansion the indices encode the guest vertex index plus a part needed for host expansion, so
   * values can exceed UINT16_MAX, up to (UINT16_MAX - 1) * 4 + 3 for point sprites. An index
   * buffer expands point sprites and rectangle lists instead of instancing, as instancing
   * performance varies widely between GPUs, for example with no different instances in the same
   * wavefront at small vertex counts per instance. Triangle strips with primitive restart rather
   * than lists let the vertex shader run once for the inner edge vertices, which matters for memory
   * export in guest shaders so 2 invocations do not write the same location. The 16 bit entries
   * serve indirection on top of single auto indexed vertices, enough even when the backend has
   * primitive reset always enabled (Metal), as auto indexed draws are limited to UINT16_MAX
   * vertices, not UINT16_MAX + 1.
   *
   * The triangle fan, line loop and quad list flags report whether the host supports those guest
   * primitive types directly or through geometry shader emulation. Debug overrides resolve in the
   * common code. The two flags without vertex shader expansion report the same for point sprites
   * and rectangle lists, and overrides do not apply to them as hosts are not required to support
   * the fallback paths, which need a different vertex shader structure for the fallback
   * HostVertexShaderTypes.
   *
   * @param full_32bit_vertex_indices_supported True when full 32 bit indices are supported.
   * @param triangle_fans_supported True when triangle fans are supported.
   * @param line_loops_supported True when line loops are supported.
   * @param quad_lists_supported True when quad lists are supported.
   * @param point_sprites_supported_without_vs_expansion True when point sprites need no expansion.
   * @param rectangle_lists_supported_without_vs_expansion True when rectangle lists need none.
   * @return True when initialization succeeded.
   */
  bool InitializeCommon(bool full_32bit_vertex_indices_supported, bool triangle_fans_supported,
                        bool line_loops_supported, bool quad_lists_supported,
                        bool point_sprites_supported_without_vs_expansion,
                        bool rectangle_lists_supported_without_vs_expansion);
  /**
   * Sets up the backend index buffer holding indices for primitive type remapping, when any
   * primitive type conversion is needed for auto indexed draws. Called from InitializeCommon, so
   * once in the processor's lifetime. The backend allocates a 4 byte aligned buffer with size_bytes
   * and calls fill_callback for its mapping if creation succeeded.
   *
   * @param size_bytes The buffer size in bytes.
   * @param fill_callback The callback that fills the mapped buffer.
   * @return True when creation succeeded.
   */
  virtual bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                            std::function<void(void*)> fill_callback) = 0;
  /** Shuts down the common state. Runs last in implementation shutdown and in the destructor. */
  void ShutdownCommon();

  /**
   * Clears the per frame cache. Runs at boundaries of converted data lifespans, preferably at the
   * end of a frame between the swap and the next draw, so access violation handlers do less work.
   */
  void ClearPerFrameCache();

  static constexpr size_t GetBuiltinIndexBufferOffsetBytes(size_t handle) { return handle; }

  /**
   * Returns the offset that moves the host pointer forward into the allocation padding so the
   * source and destination are co-aligned for SIMD. The destination allocation must have
   * REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE excess bytes. Relies on two's complement wrapping
   * overflow behavior.
   *
   * @param host_index_ptr The host index pointer.
   * @param guest_index_base The guest index base address.
   * @return The offset in bytes.
   */
  static ptrdiff_t GetSimdCoalignmentOffset(const void* host_index_ptr, uint32_t guest_index_base) {
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
    return ptrdiff_t((guest_index_base - reinterpret_cast<uintptr_t>(host_index_ptr)) &
                     (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1));
#else
    return 0;
#endif
  }

  /**
   * Requests a buffer to write the new transformed indices to. The lifetime of the returned buffer
   * must be that of the current frame.
   *
   * @param format The index format.
   * @param index_count The number of indices.
   * @param coalign_for_simd True to co-align the source and destination for SIMD.
   * @param coalignment_original_address The source address to co-align with.
   * @param backend_handle_out A handle for the backend command processor to bind the buffer.
   * @return The mapping of the buffer to write to, or nullptr on failure.
   */
  virtual void* RequestHostConvertedIndexBufferForCurrentFrame(
      xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
      uint32_t coalignment_original_address, size_t& backend_handle_out) = 0;

 private:
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
#if REX_ARCH_AMD64
  using SimdVectorU16 = simde__m128i;
  using SimdVectorU32 = simde__m128i;
  static SimdVectorU16 ReplicateU16(uint16_t value) {
    return simde_mm_set1_epi16(static_cast<int16_t>(value));
  }
  static SimdVectorU32 ReplicateU32(uint32_t value) {
    return simde_mm_set1_epi32(static_cast<int32_t>(value));
  }
  static SimdVectorU16 LoadAlignedVectorU16(const uint16_t* source) {
    return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
  }
  static SimdVectorU32 LoadAlignedVectorU32(const uint32_t* source) {
    return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(source));
  }
  static void StoreUnalignedVectorU16(uint16_t* dest, SimdVectorU16 source) {
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
  }
  static void StoreUnalignedVectorU32(uint32_t* dest, SimdVectorU32 source) {
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dest), source);
  }
#elif REX_ARCH_ARM64
  using SimdVectorU16 = uint16x8_t;
  using SimdVectorU32 = uint32x4_t;
  static SimdVectorU16 ReplicateU16(uint16_t value) { return vdupq_n_u16(value); }
  static SimdVectorU32 ReplicateU32(uint32_t value) { return vdupq_n_u32(value); }
  static SimdVectorU16 LoadAlignedVectorU16(const uint16_t* source) {
    return vld1q_u16(std::assume_aligned<sizeof(uint16x8_t)>(source));
  }
  static SimdVectorU32 LoadAlignedVectorU32(const uint32_t* source) {
    return vld1q_u32(std::assume_aligned<sizeof(uint32x4_t)>(source));
  }
  static void StoreUnalignedVectorU16(uint16_t* dest, SimdVectorU16 source) {
    vst1q_u16(dest, source);
  }
  static void StoreUnalignedVectorU32(uint32_t* dest, SimdVectorU32 source) {
    vst1q_u32(dest, source);
  }
#else
#error SIMD vector types and constant loads not specified.
#endif  // REX_ARCH_AMD64
  static_assert(sizeof(SimdVectorU16) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
                "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
                "actually used");
  static_assert(sizeof(SimdVectorU32) == REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE,
                "REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE must reflect the vector size "
                "actually used");
  static constexpr uint32_t kSimdVectorU16Elements = sizeof(SimdVectorU16) / sizeof(uint16_t);
  static constexpr uint32_t kSimdVectorU32Elements = sizeof(SimdVectorU32) / sizeof(uint32_t);
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE

  static bool IsResetUsed(const uint16_t* source, uint32_t count,
                          uint16_t reset_index_guest_endian);
  static void Get16BitResetIndexUsage(const uint16_t* source, uint32_t count,
                                      uint16_t reset_index_guest_endian,
                                      bool& is_reset_index_used_out,
                                      bool& is_ffff_used_as_vertex_index_out);
  static bool IsResetUsed(const uint32_t* source, uint32_t count, uint32_t reset_index_guest_endian,
                          uint32_t low_bits_mask_guest_endian);
  static void ReplaceResetIndex16To16(uint16_t* dest, const uint16_t* source, uint32_t count,
                                      uint16_t reset_index_guest_endian);
  /**
   * Converts 16 bit indices to 32 bit, keeping 0xFFFF as a real index and replacing the reset index
   * with 0xFFFFFFFF. For use when the reset index is not 0xFFFF and 0xFFFF is also a valid index.
   * The SIMD path compares to the reset index as uint16 getting 0 or 0xFFFF, replaces the low 16
   * bits of reset indices with 0xFFFF via OR with the comparison result, and expands to 32 bit with
   * 0xFFFF in the upper 16 bits where the comparison passed. That gives 0xFFFFFFFF for primitive
   * reset and 0x0000#### otherwise, including 0x0000FFFF if the original buffer had 0xFFFF but the
   * reset index differs.
   *
   * @param dest The destination indices.
   * @param source The source indices.
   * @param count The number of indices.
   * @param reset_index_guest_endian The reset index in guest endian.
   */
  static void ReplaceResetIndex16To24(uint32_t* dest, const uint16_t* source, uint32_t count,
                                      uint16_t reset_index_guest_endian);
  /**
   * Masks 32 bit indices to 24 bits and replaces the reset index with 0xFFFFFFFF. The reset index
   * and the low 24 bits mask are taken explicitly because the function is used 2 ways. Passthrough
   * is for when the vertex shader swaps the indices, with 32 bit indices supported on the host.
   * Then HostSwap is kNone, but the reset index and the low bits mask can be swapped to the guest
   * endian. Swapping for the host is for when the host supports only 24 bits of an index. Then
   * masking and comparison happen before applying HostSwap, and the data is swapped according to
   * HostSwap from the PowerPC big endianness to the host GPU little endianness, which matches the
   * Xenos.
   *
   * @tparam HostSwap The endian swap applied for the host.
   * @param dest The destination indices.
   * @param source The source indices.
   * @param count The number of indices.
   * @param reset_index_guest_endian The reset index in guest endian.
   * @param low_bits_mask_guest_endian The low 24 bits mask in guest endian.
   */
  template <xenos::Endian HostSwap>
  static void ReplaceResetIndex32To24(uint32_t* dest, const uint32_t* source, uint32_t count,
                                      uint32_t reset_index_guest_endian,
                                      uint32_t low_bits_mask_guest_endian) {
    // The Xenos only uses the low 24 bits of the index.
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
    while (count &&
           (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
      --count;
      uint32_t index = *(source++) & low_bits_mask_guest_endian;
      *(dest++) = index != reset_index_guest_endian ? xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
    }
    if (count >= kSimdVectorU32Elements) {
      SimdVectorU32 reset_index_guest_endian_simd = ReplicateU32(reset_index_guest_endian);
      SimdVectorU32 low_bits_mask_guest_endian_simd = ReplicateU32(low_bits_mask_guest_endian);
#if REX_ARCH_AMD64
      simde__m128i host_swap_shuffle;
      if constexpr (HostSwap != xenos::Endian::kNone) {
        host_swap_shuffle = simde_mm_set_epi32(
            static_cast<int32_t>(xenos::GPUSwap(static_cast<uint32_t>(0x0F0E0D0C), HostSwap)),
            static_cast<int32_t>(xenos::GPUSwap(static_cast<uint32_t>(0x0B0A0908), HostSwap)),
            static_cast<int32_t>(xenos::GPUSwap(static_cast<uint32_t>(0x07060504), HostSwap)),
            static_cast<int32_t>(xenos::GPUSwap(static_cast<uint32_t>(0x03020100), HostSwap)));
      }
#endif  // REX_ARCH_AMD64
      while (count >= kSimdVectorU32Elements) {
        count -= kSimdVectorU32Elements;
        // The comparison gives 0 or all ones, so the result is index | (index == reset_index).
        SimdVectorU32 source_simd = LoadAlignedVectorU32(source);
        source += kSimdVectorU32Elements;
        SimdVectorU32 result_simd;
#if REX_ARCH_AMD64
        source_simd = simde_mm_and_si128(source_simd, low_bits_mask_guest_endian_simd);
        result_simd = simde_mm_or_si128(
            source_simd, simde_mm_cmpeq_epi32(source_simd, reset_index_guest_endian_simd));
        if constexpr (HostSwap != xenos::Endian::kNone) {
          result_simd = simde_mm_shuffle_epi8(result_simd, host_swap_shuffle);
        }
#elif REX_ARCH_ARM64
        source_simd = vandq_u32(source_simd, low_bits_mask_guest_endian_simd);
        result_simd = vorrq_u32(source_simd, vceqq_u32(source_simd, reset_index_guest_endian_simd));
        if constexpr (HostSwap == xenos::Endian::k8in16) {
          result_simd = vreinterpretq_u32_u8(vrev16q_u8(vreinterpretq_u8_u32(result_simd)));
        } else if constexpr (HostSwap == xenos::Endian::k8in32) {
          result_simd = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(result_simd)));
        } else if constexpr (HostSwap == xenos::Endian::k16in32) {
          result_simd = vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(result_simd)));
        }
#else
#error SIMD ReplaceResetIndex32To24 not implemented.
#endif  // REX_ARCH_AMD64
        StoreUnalignedVectorU32(dest, result_simd);
        dest += kSimdVectorU32Elements;
      }
    }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
    while (count--) {
      uint32_t index = *(source++) & low_bits_mask_guest_endian;
      *(dest++) = index != reset_index_guest_endian ? xenos::GPUSwap(index, HostSwap) : UINT32_MAX;
    }
  }

  struct PassthroughIndexTransform {
    uint16_t operator()(uint16_t index) const { return index; }
    uint32_t operator()(uint32_t index) const { return index; }
  };
  struct To24NonSwappingIndexTransform {
    uint32_t operator()(uint32_t index) const { return index & xenos::kVertexIndexMask; }
  };
  struct To24Swapping8In16IndexTransform {
    uint32_t operator()(uint32_t index) const {
      return xenos::GPUSwap(index, xenos::Endian::k8in16) & xenos::kVertexIndexMask;
    }
  };
  struct To24Swapping8In32IndexTransform {
    uint32_t operator()(uint32_t index) const {
      return xenos::GPUSwap(index, xenos::Endian::k8in32) & xenos::kVertexIndexMask;
    }
  };
  struct To24Swapping16In32IndexTransform {
    uint32_t operator()(uint32_t index) const {
      return xenos::GPUSwap(index, xenos::Endian::k16in32) & xenos::kVertexIndexMask;
    }
  };

  static constexpr uint32_t GetTwoTriangleStripIndexCount(uint32_t strip_count) {
    return 4 * strip_count + (std::max(strip_count, UINT32_C(1)) - 1);
  }

  /**
   * Returns the index count of a triangle fan as a triangle list, ordered as (v1, v2, v0),
   * (v2, v3, v0) in Direct3D.
   *
   * @param fan_index_count The triangle fan index count.
   * @return The triangle list index count.
   */
  static constexpr uint32_t GetTriangleFanListIndexCount(uint32_t fan_index_count) {
    return fan_index_count > 2 ? (fan_index_count - 2) * 3 : 0;
  }
  template <typename Index, typename IndexTransform>
  static void TriangleFanToList(Index* dest, const Index* source, uint32_t source_index_count,
                                const IndexTransform& index_transform) {
    if (source_index_count <= 2) {
      // To match GetTriangleFanListIndexCount.
      return;
    }
    Index index_first = index_transform(source[0]);
    Index index_previous = index_transform(source[1]);
    for (uint32_t i = 2; i < source_index_count; ++i) {
      Index index_current = index_transform(source[i]);
      *(dest++) = index_previous;
      *(dest++) = index_current;
      *(dest++) = index_first;
      index_previous = index_current;
    }
  }

  /**
   * Returns the index count of a line loop as a line strip. Loops draw n lines for n input
   * vertices, so 2 vertices still draw 2 lines, and a loop of 1 vertex is ignored.
   *
   * @param loop_index_count The line loop index count.
   * @return The line strip index count.
   */
  static constexpr uint32_t GetLineLoopStripIndexCount(uint32_t loop_index_count) {
    return loop_index_count > 1 ? loop_index_count + 1 : 0;
  }
  template <typename Index, typename IndexTransform>
  static void LineLoopToStrip(Index* dest, const Index* source, uint32_t source_index_count,
                              const IndexTransform& index_transform) {
    if (source_index_count <= 1) {
      // To match GetLineLoopStripIndexCount.
      return;
    }
    Index index_first = index_transform(source[0]);
    dest[0] = index_first;
    for (uint32_t i = 1; i < source_index_count; ++i) {
      dest[i] = index_transform(source[i]);
    }
    dest[source_index_count] = index_first;
  }
  static void LineLoopToStrip(uint16_t* dest, const uint16_t* source, uint32_t source_index_count,
                              const PassthroughIndexTransform& index_transform);
  static void LineLoopToStrip(uint32_t* dest, const uint32_t* source, uint32_t source_index_count,
                              const PassthroughIndexTransform& index_transform);

  static constexpr uint32_t GetQuadListTriangleListIndexCount(uint32_t quad_list_index_count) {
    return (quad_list_index_count / 4) * 6;
  }
  template <typename Index, typename IndexTransform>
  static void QuadListToTriangleList(Index* dest, const Index* source, uint32_t source_index_count,
                                     const IndexTransform& index_transform) {
    uint32_t quad_count = source_index_count / 4;
    for (uint32_t i = 0; i < quad_count; ++i) {
      Index common_index_0 = index_transform(*(source++));
      *(dest++) = common_index_0;
      *(dest++) = index_transform(*(source++));
      Index common_index_2 = index_transform(*(source++));
      *(dest++) = common_index_2;
      *(dest++) = common_index_0;
      *(dest++) = common_index_2;
      *(dest++) = index_transform(*(source++));
    }
  }

  /**
   * Pre gathering the ranges lets the same functions convert with and without reset. It also
   * avoids a mismatch between the pre calculated post conversion index count and the actual
   * conversion if the guest modifies the index buffer between the two and adds or removes reset
   * indices.
   */
  struct SinglePrimitiveRange {
    SinglePrimitiveRange(uint32_t guest_offset, uint32_t guest_index_count,
                         uint32_t host_index_count)
        : guest_offset(guest_offset),
          guest_index_count(guest_index_count),
          host_index_count(host_index_count) {}
    uint32_t guest_offset;
    uint32_t guest_index_count;
    uint32_t host_index_count;
  };
  template <std::invocable<uint32_t> F>
  static uint32_t GetMultiPrimitiveHostIndexCountAndRanges(
      F single_primitive_guest_to_host_count, const uint16_t* source, uint32_t source_index_count,
      uint16_t reset_index_guest_endian, std::deque<SinglePrimitiveRange>& ranges_append_out);
  template <std::invocable<uint32_t> F>
  static uint32_t GetMultiPrimitiveHostIndexCountAndRanges(
      F single_primitive_guest_to_host_count, const uint32_t* source, uint32_t source_index_count,
      uint32_t reset_index_guest_endian, uint32_t low_bits_mask_guest_endian,
      std::deque<SinglePrimitiveRange>& ranges_append_out);

  template <typename Index, typename IndexTransform, typename PrimitiveRangeIterator>
  static void ConvertSinglePrimitiveRanges(Index* dest, const Index* source,
                                           xenos::PrimitiveType source_primitive_type,
                                           const IndexTransform& index_transform,
                                           PrimitiveRangeIterator ranges_beginning,
                                           PrimitiveRangeIterator ranges_end) {
    Index* dest_write_ptr = dest;
    switch (source_primitive_type) {
      case xenos::PrimitiveType::kTriangleFan:
        for (PrimitiveRangeIterator range_it = ranges_beginning; range_it != ranges_end;
             ++range_it) {
          TriangleFanToList(dest_write_ptr, source + range_it->guest_offset,
                            range_it->guest_index_count, index_transform);
          dest_write_ptr += range_it->host_index_count;
        }
        break;
      case xenos::PrimitiveType::kLineLoop:
        for (PrimitiveRangeIterator range_it = ranges_beginning; range_it != ranges_end;
             ++range_it) {
          LineLoopToStrip(dest_write_ptr, source + range_it->guest_offset,
                          range_it->guest_index_count, index_transform);
          dest_write_ptr += range_it->host_index_count;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        for (PrimitiveRangeIterator range_it = ranges_beginning; range_it != ranges_end;
             ++range_it) {
          QuadListToTriangleList(dest_write_ptr, source + range_it->guest_offset,
                                 range_it->guest_index_count, index_transform);
          dest_write_ptr += range_it->host_index_count;
        }
        break;
      default:
        REX_UNHANDLED_CASE(source_primitive_type);
    }
  }

  const xenos::RegisterFile& register_file_;
  runtime::Memory& memory_;
  SharedMemory& shared_memory_;

  bool full_32bit_vertex_indices_used_ = false;
  bool convert_triangle_fans_to_lists_ = false;
  bool convert_line_loops_to_strips_ = false;
  bool convert_quad_lists_to_triangle_lists_ = false;
  bool expand_point_sprites_in_vs_ = false;
  bool expand_rectangle_lists_in_vs_ = false;

  /** Byte offsets, used directly as handles. */
  size_t builtin_ib_offset_two_triangle_strips_ = SIZE_MAX;
  size_t builtin_ib_offset_triangle_fans_to_lists_ = SIZE_MAX;
  size_t builtin_ib_offset_quad_lists_to_triangle_lists_ = SIZE_MAX;

  std::deque<SinglePrimitiveRange> single_primitive_ranges_;

  /**
   * Converted indices are cached for reuse within a frame. Buckets are 256 KB, as the largest
   * possible guest index buffer, 0xFFFF 32 bit indices, is slightly smaller, so cache entries
   * store links within at most 2 buckets.
   */
  static constexpr uint32_t kCacheBucketSizeBytesLog2 = 18;
  static constexpr uint32_t kCacheBucketSizeBytes = static_cast<uint32_t>(1)
                                                    << kCacheBucketSizeBytesLog2;
  static constexpr uint32_t kCacheBucketCount =
      rex::align(SharedMemory::kBufferSize, kCacheBucketSizeBytes) / kCacheBucketSizeBytes;

  union CacheKey {
    uint64_t key;
    struct {
      uint32_t base;
      uint32_t count : 16;
      xenos::IndexFormat format : 1;
      xenos::Endian endian : 2;
      uint32_t is_reset_enabled : 1;
      /** kNone when the type does not change, like when only processing the reset index. */
      xenos::PrimitiveType conversion_guest_primitive_type : 6;
      /**
       * Set when the entry is for forced 32 bit guest DMA to host endian 24 bit index conversion,
       * used by non kVertex host vertex shader types on backends without full 32 bit index fetch
       * in this path.
       */
      uint32_t non_vertex_32bit_dma_to_24bit : 1;
    };

    CacheKey() : key(0) { REX_STATIC_ASSERT_SIZE(*this, sizeof(key)); }
    CacheKey(uint32_t base, uint32_t count, xenos::IndexFormat format, xenos::Endian endian,
             bool is_reset_enabled,
             xenos::PrimitiveType conversion_guest_primitive_type = xenos::PrimitiveType::kNone,
             bool non_vertex_32bit_dma_to_24bit = false) {
      // An initializer list would skip key = 0 and cannot init aliasing union members.
      key = 0;
      this->base = base;
      this->count = count;
      this->format = format;
      this->endian = endian;
      this->is_reset_enabled = is_reset_enabled;
      this->conversion_guest_primitive_type = conversion_guest_primitive_type;
      this->non_vertex_32bit_dma_to_24bit = non_vertex_32bit_dma_to_24bit;
    }

    struct Hasher {
      size_t operator()(const CacheKey& key) const { return std::hash<uint64_t>{}(key.key); }
    };
    bool operator==(const CacheKey& other_key) const { return key == other_key.key; }

    uint32_t GetSizeBytes() const {
      return count * (format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t));
    }
  };

  /**
   * Subset of ConversionResult that can be reused for different primitive types if the same result
   * applies irrespective of one, like when only processing the reset index.
   */
  struct CachedResult {
    uint32_t host_draw_vertex_count;
    ProcessedIndexBufferType index_buffer_type;
    xenos::IndexFormat host_index_format;
    xenos::Endian host_shader_index_endian;
    bool host_primitive_reset_enabled;
    size_t host_index_buffer_handle;
  };

  struct CacheEntry {
    static_assert(UINT16_MAX * sizeof(uint32_t) <=
                      (static_cast<size_t>(1) << kCacheBucketSizeBytesLog2),
                  "Assuming that primitive processor cache entries need to store to the "
                  "previous and to the next entries only within up to 2 buckets, so the "
                  "size of the cache buckets must be not smaller than the maximum guest "
                  "index buffer size");
    union {
      size_t free_next;
      size_t buckets_prev[2];
    };
    size_t buckets_next[2];
    CacheKey key;
    CachedResult result;
    static uint32_t GetBucketCount(CacheKey key) {
      uint32_t count = ((key.base + (key.GetSizeBytes() - 1)) >> kCacheBucketSizeBytesLog2) -
                       (key.base >> kCacheBucketSizeBytesLog2) + 1;
      REX_ASSERT(count <= 2, "Cache entries only store list links within two buckets");
      return count;
    }
    uint32_t GetBucketCount() const { return GetBucketCount(key); }
  };

  /**
   * A cache transaction performs a few operations in a RAII way, so processing may return an error
   * for any reason without cleaning up cache_currently_processing_base_ and size_bytes_ explicitly.
   *
   * Initialization looks up previously processed indices in the cache. If not found, it begins
   * adding a new entry by marking the range as being processed and enabling an access callback for
   * the range. Marking makes race handling slightly safer. If invalidation happens during the
   * transaction, outside a global critical region lock since processing may take long, the new
   * entry is not stored as it is already invalid at completion. The new result is set after
   * processing when not found previously. Completion stores the new entry in the cache if the range
   * was not invalidated during the transaction.
   *
   * An entry found in the cache (GetFoundResult is non null) MUST be used instead of processing, as
   * this class cannot replace existing entries.
   */
  class CacheTransaction final {
   public:
    CacheTransaction(PrimitiveProcessor& processor, CacheKey key);
    const CachedResult* GetFoundResult() const {
      return result_type_ == ResultType::kExisting ? &result_ : nullptr;
    }
    void SetNewResult(const CachedResult& new_result) {
      REX_ASSERT(result_type_ != ResultType::kExisting);
      result_ = new_result;
      result_type_ = ResultType::kNewSet;
    }
    ~CacheTransaction();

   private:
    PrimitiveProcessor& processor_;
    /**
     * If key_.count == 0 the transaction does nothing, as it is pointless for empty ranges and
     * unsafe to get the end pointer without special logic. count == 0 also indicates a vertex count
     * below the cache usage threshold.
     */
    CacheKey key_;
    CachedResult result_;
    enum class ResultType {
      kNewUnset,
      kNewSet,
      kExisting,
    };
    ResultType result_type_ = ResultType::kNewUnset;
  };

  std::deque<CacheEntry> cache_entry_pool_;

  void* memory_invalidation_callback_handle_ = nullptr;

  std::mutex cache_mutex_;
  // Modified by both the processor and the invalidation callback.
  std::unordered_map<CacheKey, size_t, CacheKey::Hasher> cache_map_;
  /**
   * The conversion runs with the lock released since it may take long. If the region is
   * invalidated during the conversion, the entry is not added to the cache. Modified by the
   * processor, read by the invalidation callback.
   */
  uint32_t cache_currently_processing_base_ = 0;
  /** 0 when not in a cache transaction that has not found an existing entry yet. */
  uint32_t cache_currently_processing_size_bytes_ = 0;
  // Modified by both the processor and the invalidation callback.
  size_t cache_bucket_free_first_entry_ = SIZE_MAX;
  // Modified by both the processor and the invalidation callback.
  uint64_t cache_buckets_non_empty_l1_[(kCacheBucketCount + 63) / 64] = {};
  /**
   * For faster memory invalidation, whether any bit is set in each cache_buckets_non_empty_l1_.
   * Modified by both the processor and the invalidation callback.
   */
  uint64_t cache_buckets_non_empty_l2_[(kCacheBucketCount + (64 * 64 - 1)) / (64 * 64)] = {};
  /**
   * Updates the L2 bit of a bucket group. Requires a global critical region.
   *
   * @param bucket_index_div_64 The bucket index divided by 64.
   * @param cache_lock The held cache lock.
   */
  void UpdateCacheBucketsNonEmptyL2(
      uint32_t bucket_index_div_64,
      [[maybe_unused]] const std::lock_guard<std::mutex>& cache_lock) {
    uint64_t& cache_buckets_non_empty_l2_ref =
        cache_buckets_non_empty_l2_[bucket_index_div_64 >> 6];
    uint64_t cache_buckets_non_empty_l2_bit = static_cast<uint64_t>(1)
                                              << (bucket_index_div_64 & 63);
    if (cache_buckets_non_empty_l1_[bucket_index_div_64]) {
      cache_buckets_non_empty_l2_ref |= cache_buckets_non_empty_l2_bit;
    } else {
      cache_buckets_non_empty_l2_ref &= ~cache_buckets_non_empty_l2_bit;
    }
  }
  /**
   * cache_buckets_non_empty_l1_, kept in sync with cache_buckets_non_empty_l2_, indicates whether
   * each entry is non empty, for faster clearing, as there is no special index here for an empty
   * entry. Huge, so it is the last in the class. Modified by both the processor and the
   * invalidation callback.
   */
  size_t cache_bucket_first_entries_[kCacheBucketCount];
  static std::pair<uint32_t, uint32_t> MemoryInvalidationCallbackThunk(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);
};

}  // namespace rex::gpu
