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

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/primitive_processor.h"
#include <rex/xenos/register_file.h>
#include <rex/xenos/registers.h>
#include <rex/xenos/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/types.h>

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu {

PrimitiveProcessor::~PrimitiveProcessor() {
  ShutdownCommon();
}

bool PrimitiveProcessor::InitializeCommon(bool full_32bit_vertex_indices_supported,
                                          bool triangle_fans_supported, bool line_loops_supported,
                                          bool quad_lists_supported,
                                          bool point_sprites_supported_without_vs_expansion,
                                          bool rectangle_lists_supported_without_vs_expansion) {
  full_32bit_vertex_indices_used_ = full_32bit_vertex_indices_supported;
  convert_triangle_fans_to_lists_ =
      !triangle_fans_supported ||
      REXCVAR_GET(::rex::gpu::cvars::force_convert_triangle_fans_to_lists);
  convert_line_loops_to_strips_ =
      !line_loops_supported || REXCVAR_GET(::rex::gpu::cvars::force_convert_line_loops_to_strips);
  convert_quad_lists_to_triangle_lists_ =
      !quad_lists_supported ||
      REXCVAR_GET(::rex::gpu::cvars::force_convert_quad_lists_to_triangle_lists);
  expand_point_sprites_in_vs_ = !point_sprites_supported_without_vs_expansion;
  expand_rectangle_lists_in_vs_ = !rectangle_lists_supported_without_vs_expansion;

  size_t builtin_index_buffer_size = 0;
  u32 builtin_ib_two_triangle_strip_count = 0;
  if (expand_point_sprites_in_vs_) {
    builtin_ib_two_triangle_strip_count =
        std::max(static_cast<u32>(UINT16_MAX), builtin_ib_two_triangle_strip_count);
  }
  if (expand_rectangle_lists_in_vs_) {
    builtin_ib_two_triangle_strip_count =
        std::max(static_cast<u32>(UINT16_MAX / 3), builtin_ib_two_triangle_strip_count);
  }
  if (builtin_ib_two_triangle_strip_count) {
    builtin_ib_offset_two_triangle_strips_ = builtin_index_buffer_size;
    builtin_index_buffer_size +=
        sizeof(u32) * GetTwoTriangleStripIndexCount(builtin_ib_two_triangle_strip_count);
  } else {
    builtin_ib_offset_two_triangle_strips_ = SIZE_MAX;
  }
  if (convert_triangle_fans_to_lists_) {
    builtin_ib_offset_triangle_fans_to_lists_ = builtin_index_buffer_size;
    builtin_index_buffer_size += sizeof(u16) * GetTriangleFanListIndexCount(UINT16_MAX);
  } else {
    builtin_ib_offset_triangle_fans_to_lists_ = SIZE_MAX;
  }
  if (convert_quad_lists_to_triangle_lists_) {
    builtin_ib_offset_quad_lists_to_triangle_lists_ = builtin_index_buffer_size;
    builtin_index_buffer_size += sizeof(u16) * GetQuadListTriangleListIndexCount(UINT16_MAX);
  } else {
    builtin_ib_offset_quad_lists_to_triangle_lists_ = SIZE_MAX;
  }
  if (builtin_index_buffer_size) {
    if (!InitializeBuiltinIndexBuffer(
            builtin_index_buffer_size, [this, builtin_ib_two_triangle_strip_count](void* mapping) {
              if (builtin_ib_offset_two_triangle_strips_ != SIZE_MAX) {
                u32* two_triangle_strip_ptr = reinterpret_cast<u32*>(
                    static_cast<u8*>(mapping) + builtin_ib_offset_two_triangle_strips_);
                for (u32 i = 0; i < builtin_ib_two_triangle_strip_count; ++i) {
                  if (i) {
                    *(two_triangle_strip_ptr++) = UINT32_MAX;
                  }
                  // Host vertex index in the low 2 bits, guest primitive index above.
                  u32 two_triangle_strip_first_index = i << 2;
                  for (u32 j = 0; j < 4; ++j) {
                    *(two_triangle_strip_ptr++) = two_triangle_strip_first_index + j;
                  }
                }
              }
              if (builtin_ib_offset_triangle_fans_to_lists_ != SIZE_MAX) {
                u16* triangle_list_ptr = reinterpret_cast<u16*>(
                    static_cast<u8*>(mapping) + builtin_ib_offset_triangle_fans_to_lists_);
                for (u32 i = 2; i < UINT16_MAX; ++i) {
                  *(triangle_list_ptr++) = static_cast<u16>(i - 1);
                  *(triangle_list_ptr++) = static_cast<u16>(i);
                  *(triangle_list_ptr++) = 0;
                }
              }
              if (builtin_ib_offset_quad_lists_to_triangle_lists_ != SIZE_MAX) {
                u16* triangle_list_ptr = reinterpret_cast<u16*>(
                    static_cast<u8*>(mapping) + builtin_ib_offset_quad_lists_to_triangle_lists_);
                for (u32 i = 0; i < UINT16_MAX / 4; ++i) {
                  u16 quad_first_index = static_cast<u16>(i * 4);
                  *(triangle_list_ptr++) = quad_first_index;
                  *(triangle_list_ptr++) = quad_first_index + 1;
                  *(triangle_list_ptr++) = quad_first_index + 2;
                  *(triangle_list_ptr++) = quad_first_index;
                  *(triangle_list_ptr++) = quad_first_index + 2;
                  *(triangle_list_ptr++) = quad_first_index + 3;
                }
              }
            })) {
      ShutdownCommon();
      return false;
    }
  }

  return true;
}

void PrimitiveProcessor::ShutdownCommon() {
  if (memory_invalidation_callback_handle_) {
    {
      std::lock_guard<std::mutex> cache_lock(cache_mutex_);
      cache_map_.clear();
      cache_bucket_free_first_entry_ = SIZE_MAX;
      std::memset(cache_buckets_non_empty_l1_, 0, sizeof(cache_buckets_non_empty_l1_));
      std::memset(cache_buckets_non_empty_l2_, 0, sizeof(cache_buckets_non_empty_l2_));
    }
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
    cache_entry_pool_.clear();
  }
}

void PrimitiveProcessor::ClearPerFrameCache() {
  if (!memory_invalidation_callback_handle_) {
    return;
  }
  std::lock_guard<std::mutex> cache_lock(cache_mutex_);
  for (const std::pair<const CacheKey, size_t>& cache_map_entry : cache_map_) {
    cache_entry_pool_[cache_map_entry.second].free_next = cache_bucket_free_first_entry_;
    cache_bucket_free_first_entry_ = cache_map_entry.second;
  }
  cache_map_.clear();
  std::memset(cache_buckets_non_empty_l1_, 0, sizeof(cache_buckets_non_empty_l1_));
  std::memset(cache_buckets_non_empty_l2_, 0, sizeof(cache_buckets_non_empty_l2_));
}

bool PrimitiveProcessor::Process(ProcessingResult& result_out) {
  REX_PROFILE_SCOPE("gpu");

  const xenos::RegisterFile& regs = register_file_;
  auto vgt_draw_initiator = regs.Get<xenos::reg::VGT_DRAW_INITIATOR>();

  // VGT_OUTPUT_PATH_CNTL only applies in the explicit major mode, titles leave it set otherwise.
  xenos::PrimitiveType guest_primitive_type = vgt_draw_initiator.prim_type;
  xenos::PrimitiveType host_primitive_type = guest_primitive_type;
  bool tessellation_enabled =
      xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type) &&
      regs.Get<xenos::reg::VGT_OUTPUT_PATH_CNTL>().path_select ==
          xenos::VGTOutputPath::kTessellationEnable;
  xenos::TessellationMode tessellation_mode = regs.Get<xenos::reg::VGT_HOS_CNTL>().tess_mode;
  Shader::HostVertexShaderType host_vertex_shader_type;
  if (tessellation_enabled) {
    host_vertex_shader_type = Shader::HostVertexShaderType(-1);
    switch (guest_primitive_type) {
      case xenos::PrimitiveType::kTriangleList:
        // Triangle strips and fans would need conversion to triangle lists and are unsupported.
        switch (tessellation_mode) {
          case xenos::TessellationMode::kDiscrete:
          case xenos::TessellationMode::kContinuous:
            host_vertex_shader_type = Shader::HostVertexShaderType::kTriangleDomainCPIndexed;
            break;
          default:
            break;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        switch (tessellation_mode) {
          // Quad strips would need conversion to quad lists and are unsupported.
          case xenos::TessellationMode::kDiscrete:
          case xenos::TessellationMode::kContinuous:
            host_vertex_shader_type = Shader::HostVertexShaderType::kQuadDomainCPIndexed;
            break;
          default:
            break;
        }
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        host_vertex_shader_type = Shader::HostVertexShaderType::kTriangleDomainPatchIndexed;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        host_vertex_shader_type = Shader::HostVertexShaderType::kQuadDomainPatchIndexed;
        break;
      default:
        break;
    }
    if (host_vertex_shader_type == Shader::HostVertexShaderType(-1)) {
      REXLOG_ERROR(gpu,
                   "Primitive processor: Unsupported tessellation mode {} for primitive "
                   "type {}. Report the title to ReXGlue developers!",
                   static_cast<u32>(tessellation_mode), static_cast<u32>(guest_primitive_type));
      REX_ASSERT_ALWAYS();
      return false;
    }
  } else {
    host_vertex_shader_type = Shader::HostVertexShaderType::kVertex;
    switch (guest_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        if (expand_point_sprites_in_vs_) {
          host_primitive_type = xenos::PrimitiveType::kTriangleStrip;
          host_vertex_shader_type = Shader::HostVertexShaderType::kPointListAsTriangleStrip;
        }
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTriangleStrip:
        // Supported natively on all backends.
        break;
      case xenos::PrimitiveType::kRectangleList:
        if (expand_rectangle_lists_in_vs_) {
          host_primitive_type = xenos::PrimitiveType::kTriangleStrip;
          host_vertex_shader_type = Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
        }
        break;
      case xenos::PrimitiveType::kTriangleFan:
        if (convert_triangle_fans_to_lists_) {
          host_primitive_type = xenos::PrimitiveType::kTriangleList;
        }
        break;
      case xenos::PrimitiveType::kLineLoop:
        if (convert_line_loops_to_strips_) {
          host_primitive_type = xenos::PrimitiveType::kLineStrip;
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        if (convert_quad_lists_to_triangle_lists_) {
          host_primitive_type = xenos::PrimitiveType::kTriangleList;
        }
        break;
      default:
        REXLOG_ERROR(gpu,
                     "Primitive processor: Unsupported primitive type {}. Report the "
                     "title to ReXGlue developers!",
                     static_cast<u32>(guest_primitive_type));
        REX_ASSERT_ALWAYS();
        return false;
    }
  }

  u32 guest_draw_vertex_count = vgt_draw_initiator.num_indices;
  auto vgt_dma_size = regs.Get<xenos::reg::VGT_DMA_SIZE>();
  if (vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA &&
      guest_draw_vertex_count > vgt_dma_size.num_words) {
    REXLOG_WARN(gpu,
                "Primitive processor: {} vertices attempted to be drawn with an index "
                "buffer only containing {}. Should be fetching zero indices instead of "
                "overflowing ones, but this is a rare situation, so not handled yet. "
                "Report the title to ReXGlue developers!",
                guest_draw_vertex_count, static_cast<u32>(vgt_dma_size.num_words));
    guest_draw_vertex_count = vgt_dma_size.num_words;
  }
  u32 line_loop_closing_index = 0;
  u32 guest_index_base = 0, guest_index_buffer_needed_bytes = 0;
  CachedResult cacheable;
  cacheable.host_draw_vertex_count = guest_draw_vertex_count;
  cacheable.host_primitive_reset_enabled = false;
  cacheable.host_index_buffer_handle = SIZE_MAX;
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    u32 primitive_count = guest_draw_vertex_count;
    if (host_vertex_shader_type == Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
      primitive_count /= 3;
    }
    cacheable.host_draw_vertex_count = GetTwoTriangleStripIndexCount(primitive_count);
    cacheable.host_index_format = xenos::IndexFormat::kInt32;
    cacheable.host_primitive_reset_enabled = true;
    REX_ASSERT(builtin_ib_offset_two_triangle_strips_ != SIZE_MAX);
    cacheable.host_index_buffer_handle = builtin_ib_offset_two_triangle_strips_;
    if (vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex) {
      cacheable.index_buffer_type = ProcessedIndexBufferType::kHostBuiltinForAuto;
      cacheable.host_shader_index_endian = xenos::Endian::kNone;
    } else {
      REX_ASSERT(vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA);
      if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA) {
        REX_FAIL(gpu, false,
                 "Primitive processor: Unsupported vertex index source {}. Report "
                 "the title to ReXGlue developers!",
                 static_cast<u32>(vgt_draw_initiator.source_select));
      }
      xenos::IndexFormat guest_index_format = vgt_draw_initiator.index_size;
      cacheable.index_buffer_type = ProcessedIndexBufferType::kHostBuiltinForDMA;
      xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
      if (guest_index_format == xenos::IndexFormat::kInt16 &&
          (guest_index_endian != xenos::Endian::kNone &&
           guest_index_endian != xenos::Endian::k8in16)) {
        REXLOG_WARN(gpu,
                    "Primitive processor: 32-bit endian swap mode {} is used for "
                    "16-bit indices. This shouldn't normally be happening, but report "
                    "the title to ReXGlue developers for investigation of the intended "
                    "behavior (ignore or actually swap across adjacent indices)! "
                    "Currently disabling the swap for 16-and-32 and replacing 8-in-32 "
                    "with 8-in-16.",
                    static_cast<u32>(guest_index_endian));
        guest_index_endian = guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                                         : xenos::Endian::kNone;
      }
      cacheable.host_shader_index_endian = guest_index_endian;
      u32 index_size_log2 = guest_index_format == xenos::IndexFormat::kInt16 ? 1 : 2;
      // The base is already aligned, masked here for safety.
      guest_index_base = regs[std::to_underlying(xenos::Register::VGT_DMA_BASE)] &
                         ~static_cast<u32>((1 << index_size_log2) - 1);
      guest_index_buffer_needed_bytes = guest_draw_vertex_count << index_size_log2;
      if (guest_index_base > SharedMemory::kBufferSize ||
          SharedMemory::kBufferSize - guest_index_base < guest_index_buffer_needed_bytes) {
        REXLOG_ERROR(gpu,
                     "Primitive processor: Index buffer at 0x{:08X}, 0x{:X} bytes "
                     "required, is out of the physical memory bounds",
                     guest_index_base, guest_index_buffer_needed_bytes);
        REX_ASSERT_ALWAYS();
        return false;
      }
    }
  } else if (vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex) {
    if (tessellation_enabled && tessellation_mode == xenos::TessellationMode::kAdaptive) {
      REXLOG_ERROR(gpu,
                   "Primitive processor: Adaptive tessellation requires 32-bit "
                   "floating-point edge tessellation factors in the index buffer, but "
                   "no index buffer is provided by the guest.");
      REX_ASSERT_ALWAYS();
      return false;
    }
    cacheable.host_index_format = xenos::IndexFormat::kInt16;
    cacheable.host_shader_index_endian = xenos::Endian::kNone;
    cacheable.host_primitive_reset_enabled = false;
    cacheable.index_buffer_type = ProcessedIndexBufferType::kNone;
    if (host_primitive_type != guest_primitive_type) {
      switch (guest_primitive_type) {
        case xenos::PrimitiveType::kTriangleFan:
          REX_ASSERT(host_primitive_type == xenos::PrimitiveType::kTriangleList);
          cacheable.host_draw_vertex_count =
              GetTriangleFanListIndexCount(cacheable.host_draw_vertex_count);
          cacheable.index_buffer_type = ProcessedIndexBufferType::kHostBuiltinForAuto;
          REX_ASSERT(builtin_ib_offset_triangle_fans_to_lists_ != SIZE_MAX);
          cacheable.host_index_buffer_handle = builtin_ib_offset_triangle_fans_to_lists_;
          break;
        case xenos::PrimitiveType::kLineLoop:
          // The extra closing index counts as 0 in the vertex shaders.
          REX_ASSERT(host_primitive_type == xenos::PrimitiveType::kLineStrip);
          cacheable.host_draw_vertex_count =
              GetLineLoopStripIndexCount(cacheable.host_draw_vertex_count);
          if (cacheable.host_draw_vertex_count) {
            line_loop_closing_index = cacheable.host_draw_vertex_count - 1;
          }
          break;
        case xenos::PrimitiveType::kQuadList:
          REX_ASSERT(host_primitive_type == xenos::PrimitiveType::kTriangleList);
          cacheable.host_draw_vertex_count =
              GetQuadListTriangleListIndexCount(cacheable.host_draw_vertex_count);
          cacheable.index_buffer_type = ProcessedIndexBufferType::kHostBuiltinForAuto;
          REX_ASSERT(builtin_ib_offset_quad_lists_to_triangle_lists_ != SIZE_MAX);
          cacheable.host_index_buffer_handle = builtin_ib_offset_quad_lists_to_triangle_lists_;
          break;
        default:
          REX_ASSERT_ALWAYS();
          return false;
      }
    }
  } else {
    REX_ASSERT(vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA);
    if (vgt_draw_initiator.source_select != xenos::SourceSelect::kDMA) {
      REX_FAIL(gpu, false,
               "Primitive processor: Unsupported vertex index source {}. Report the "
               "title to ReXGlue developers!",
               static_cast<u32>(vgt_draw_initiator.source_select));
    }
    xenos::IndexFormat guest_index_format = vgt_draw_initiator.index_size;
    cacheable.host_index_format = guest_index_format;
    xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
    if (guest_index_format == xenos::IndexFormat::kInt16 &&
        (guest_index_endian != xenos::Endian::kNone &&
         guest_index_endian != xenos::Endian::k8in16)) {
      REXLOG_WARN(gpu,
                  "Primitive processor: 32-bit endian swap mode {} is used for 16-bit "
                  "indices. This shouldn't normally be happening, but report the title "
                  "to ReXGlue developers for investigation of the intended behavior "
                  "(ignore or actually swap across adjacent indices)! Currently "
                  "disabling the swap for 16-and-32 and replacing 8-in-32 with "
                  "8-in-16.",
                  static_cast<u32>(guest_index_endian));
      guest_index_endian = guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                                       : xenos::Endian::kNone;
    }
    bool guest_primitive_reset_enabled = false;
    u32 guest_primitive_reset_index_guest_endian = 0;
    if (tessellation_enabled && tessellation_mode == xenos::TessellationMode::kAdaptive) {
      // Adaptive tessellation holds 32 bit float edge factors, not indices, so there is no reset.
      if (guest_index_format != xenos::IndexFormat::kInt32) {
        REXLOG_ERROR(gpu,
                     "Primitive processor: Adaptive tessellation requires 32-bit "
                     "floating-point edge tessellation factors in the index buffer, but "
                     "16-bit index buffer is provided by the guest.");
        REX_ASSERT_ALWAYS();
        return false;
      }
    } else {
      if (regs.Get<xenos::reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
        switch (guest_primitive_type) {
          case xenos::PrimitiveType::kLineStrip:
          case xenos::PrimitiveType::kTriangleFan:
          case xenos::PrimitiveType::kTriangleStrip:
          case xenos::PrimitiveType::kLineLoop:
          case xenos::PrimitiveType::kQuadStrip:
          case xenos::PrimitiveType::kPolygon:
          case xenos::PrimitiveType::k2DLineStrip:
          case xenos::PrimitiveType::k2DTriStrip:
            guest_primitive_reset_index_guest_endian =
                xenos::GPUSwap(regs.Get<xenos::reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx,
                               guest_index_endian);
            guest_primitive_reset_enabled =
                guest_index_format == xenos::IndexFormat::kInt16
                    ? guest_primitive_reset_index_guest_endian <= UINT16_MAX
                    : true;
            break;
          default:
            // Vulkan forbids primitive restart for lists and Direct3D 12 leaves it undefined.
            break;
        }
      }
    }

    u32 index_size_log2 = guest_index_format == xenos::IndexFormat::kInt16 ? 1 : 2;
    // The base is already aligned, masked here for safety.
    guest_index_base = regs[std::to_underlying(xenos::Register::VGT_DMA_BASE)] &
                       ~static_cast<u32>((1 << index_size_log2) - 1);
    guest_index_buffer_needed_bytes = guest_draw_vertex_count << index_size_log2;
    if (guest_index_base > SharedMemory::kBufferSize ||
        SharedMemory::kBufferSize - guest_index_base < guest_index_buffer_needed_bytes) {
      REXLOG_ERROR(gpu,
                   "Primitive processor: Index buffer at 0x{:08X}, 0x{:X} bytes "
                   "required, is out of the physical memory bounds",
                   guest_index_base, guest_index_buffer_needed_bytes);
      REX_ASSERT_ALWAYS();
      return false;
    }

    cacheable.host_index_format = guest_index_format;
    cacheable.host_shader_index_endian = guest_index_endian;
    u32 guest_index_mask_guest_endian = guest_index_format == xenos::IndexFormat::kInt16
                                            ? UINT16_MAX
                                            : GPUSwap(xenos::kVertexIndexMask, guest_index_endian);
    if (host_primitive_type != guest_primitive_type) {
      // Reset and swap happen in conversion, as indirection only reads shared memory.
      CacheTransaction cache_transaction(
          *this, CacheKey(guest_index_base, guest_draw_vertex_count, guest_index_format,
                          guest_index_endian, guest_primitive_reset_enabled, guest_primitive_type));
      if (cache_transaction.GetFoundResult()) {
        cacheable = *cache_transaction.GetFoundResult();
      } else {
        const void* guest_indices_ptr = memory_.TranslatePhysical(guest_index_base);
        cacheable.index_buffer_type = ProcessedIndexBufferType::kHostConverted;
        cacheable.host_primitive_reset_enabled = false;
        std::function<u32(u32)> host_index_count_getter;
        switch (guest_primitive_type) {
          case xenos::PrimitiveType::kTriangleFan:
            host_index_count_getter = GetTriangleFanListIndexCount;
            break;
          case xenos::PrimitiveType::kLineLoop:
            host_index_count_getter = GetLineLoopStripIndexCount;
            break;
          case xenos::PrimitiveType::kQuadList:
            host_index_count_getter = GetQuadListTriangleListIndexCount;
            break;
          default:
            REX_UNHANDLED_CASE(guest_primitive_type);
            return false;
        }
        single_primitive_ranges_.clear();
        if (guest_index_format == xenos::IndexFormat::kInt16) {
          auto guest_indices = reinterpret_cast<const u16*>(guest_indices_ptr);
          if (guest_primitive_reset_enabled &&
              IsResetUsed(guest_indices, guest_draw_vertex_count,
                          guest_primitive_reset_index_guest_endian)) {
            cacheable.host_draw_vertex_count = GetMultiPrimitiveHostIndexCountAndRanges(
                host_index_count_getter, guest_indices, guest_draw_vertex_count,
                guest_primitive_reset_index_guest_endian, single_primitive_ranges_);
          } else {
            cacheable.host_draw_vertex_count = host_index_count_getter(guest_draw_vertex_count);
            single_primitive_ranges_.emplace_back(0, guest_draw_vertex_count,
                                                  cacheable.host_draw_vertex_count);
          }
          auto host_indices = reinterpret_cast<u16*>(RequestHostConvertedIndexBufferForCurrentFrame(
              xenos::IndexFormat::kInt16, cacheable.host_draw_vertex_count, false, guest_index_base,
              cacheable.host_index_buffer_handle));
          if (!host_indices) {
            return false;
          }
          ConvertSinglePrimitiveRanges(
              host_indices, guest_indices, guest_primitive_type, PassthroughIndexTransform(),
              single_primitive_ranges_.cbegin(), single_primitive_ranges_.cend());
        } else {
          auto guest_indices = reinterpret_cast<const u32*>(guest_indices_ptr);
          if (guest_primitive_reset_enabled && IsResetUsed(guest_indices, guest_draw_vertex_count,
                                                           guest_primitive_reset_index_guest_endian,
                                                           guest_index_mask_guest_endian)) {
            cacheable.host_draw_vertex_count = GetMultiPrimitiveHostIndexCountAndRanges(
                host_index_count_getter, guest_indices, guest_draw_vertex_count,
                guest_primitive_reset_index_guest_endian, guest_index_mask_guest_endian,
                single_primitive_ranges_);
          } else {
            cacheable.host_draw_vertex_count = host_index_count_getter(guest_draw_vertex_count);
            single_primitive_ranges_.emplace_back(0, guest_draw_vertex_count,
                                                  cacheable.host_draw_vertex_count);
          }
          auto host_indices = reinterpret_cast<u32*>(RequestHostConvertedIndexBufferForCurrentFrame(
              xenos::IndexFormat::kInt32, cacheable.host_draw_vertex_count, false, guest_index_base,
              cacheable.host_index_buffer_handle));
          if (!host_indices) {
            return false;
          }
          auto single_primitive_ranges_beginning = single_primitive_ranges_.cbegin();
          auto single_primitive_ranges_end = single_primitive_ranges_.cend();
          if (full_32bit_vertex_indices_used_) {
            ConvertSinglePrimitiveRanges(
                host_indices, guest_indices, guest_primitive_type, PassthroughIndexTransform(),
                single_primitive_ranges_beginning, single_primitive_ranges_end);
          } else {
            switch (guest_index_endian) {
              case xenos::Endian::kNone:
                ConvertSinglePrimitiveRanges(host_indices, guest_indices, guest_primitive_type,
                                             To24NonSwappingIndexTransform(),
                                             single_primitive_ranges_beginning,
                                             single_primitive_ranges_end);
                break;
              case xenos::Endian::k8in16:
                ConvertSinglePrimitiveRanges(host_indices, guest_indices, guest_primitive_type,
                                             To24Swapping8In16IndexTransform(),
                                             single_primitive_ranges_beginning,
                                             single_primitive_ranges_end);
                break;
              case xenos::Endian::k8in32:
                ConvertSinglePrimitiveRanges(host_indices, guest_indices, guest_primitive_type,
                                             To24Swapping8In32IndexTransform(),
                                             single_primitive_ranges_beginning,
                                             single_primitive_ranges_end);
                break;
              case xenos::Endian::k16in32:
                ConvertSinglePrimitiveRanges(host_indices, guest_indices, guest_primitive_type,
                                             To24Swapping16In32IndexTransform(),
                                             single_primitive_ranges_beginning,
                                             single_primitive_ranges_end);
                break;
              default:
                REX_UNHANDLED_CASE(guest_index_endian);
                return false;
            }
            cacheable.host_shader_index_endian = xenos::Endian::kNone;
          }
        }
        cache_transaction.SetNewResult(cacheable);
        cache_transaction.Commit();
      }
    } else {
      cacheable.host_draw_vertex_count = guest_draw_vertex_count;
      cacheable.index_buffer_type = ProcessedIndexBufferType::kGuestDMA;
      cacheable.host_primitive_reset_enabled = guest_primitive_reset_enabled;
      if (guest_primitive_reset_enabled) {
        if (guest_index_format == xenos::IndexFormat::kInt16) {
          // 16 bit indices need no indirection even without full 32 bit index support.
          if (guest_primitive_reset_index_guest_endian != UINT16_MAX) {
            // The cache key omits the primitive type, as only the reset index is replaced.
            CacheTransaction cache_transaction(
                *this, CacheKey(guest_index_base, guest_draw_vertex_count, guest_index_format,
                                guest_index_endian, guest_primitive_reset_enabled));
            if (cache_transaction.GetFoundResult()) {
              cacheable = *cache_transaction.GetFoundResult();
            } else {
              auto guest_indices = memory_.TranslatePhysical<const u16*>(guest_index_base);
              bool is_reset_index_used, is_ffff_used_as_vertex_index;
              Get16BitResetIndexUsage(guest_indices, guest_draw_vertex_count,
                                      guest_primitive_reset_index_guest_endian, is_reset_index_used,
                                      is_ffff_used_as_vertex_index);
              if (is_reset_index_used) {
                cacheable.index_buffer_type = ProcessedIndexBufferType::kHostConverted;
                cacheable.host_index_format = is_ffff_used_as_vertex_index
                                                  ? xenos::IndexFormat::kInt32
                                                  : xenos::IndexFormat::kInt16;
                void* host_indices_ptr = RequestHostConvertedIndexBufferForCurrentFrame(
                    cacheable.host_index_format, guest_draw_vertex_count, true, guest_index_base,
                    cacheable.host_index_buffer_handle);
                if (!host_indices_ptr) {
                  return false;
                }
                if (is_ffff_used_as_vertex_index) {
                  ReplaceResetIndex16To24(reinterpret_cast<u32*>(host_indices_ptr), guest_indices,
                                          guest_draw_vertex_count,
                                          guest_primitive_reset_index_guest_endian);
                } else {
                  ReplaceResetIndex16To16(reinterpret_cast<u16*>(host_indices_ptr), guest_indices,
                                          guest_draw_vertex_count,
                                          guest_primitive_reset_index_guest_endian);
                }
              }
              cache_transaction.SetNewResult(cacheable);
              cache_transaction.Commit();
            }
          }
        } else {
          // The cache key omits the primitive type, as only the reset index is replaced.
          CacheTransaction cache_transaction(
              *this, CacheKey(guest_index_base, guest_draw_vertex_count, guest_index_format,
                              guest_index_endian, guest_primitive_reset_enabled));
          if (cache_transaction.GetFoundResult()) {
            cacheable = *cache_transaction.GetFoundResult();
          } else {
            auto guest_indices = memory_.TranslatePhysical<const u32*>(guest_index_base);
            if (IsResetUsed(guest_indices, guest_draw_vertex_count,
                            guest_primitive_reset_index_guest_endian,
                            guest_index_mask_guest_endian)) {
              cacheable.index_buffer_type = ProcessedIndexBufferType::kHostConverted;
              auto host_indices =
                  reinterpret_cast<u32*>(RequestHostConvertedIndexBufferForCurrentFrame(
                      xenos::IndexFormat::kInt32, guest_draw_vertex_count, true, guest_index_base,
                      cacheable.host_index_buffer_handle));
              if (!host_indices) {
                return false;
              }
              if (full_32bit_vertex_indices_used_ || guest_index_endian == xenos::Endian::kNone) {
                ReplaceResetIndex32To24<xenos::Endian::kNone>(
                    host_indices, guest_indices, guest_draw_vertex_count,
                    guest_primitive_reset_index_guest_endian, guest_index_mask_guest_endian);
              } else if (guest_index_endian == xenos::Endian::k8in16) {
                ReplaceResetIndex32To24<xenos::Endian::k8in16>(
                    host_indices, guest_indices, guest_draw_vertex_count,
                    guest_primitive_reset_index_guest_endian, guest_index_mask_guest_endian);
              } else if (guest_index_endian == xenos::Endian::k8in32) {
                ReplaceResetIndex32To24<xenos::Endian::k8in32>(
                    host_indices, guest_indices, guest_draw_vertex_count,
                    guest_primitive_reset_index_guest_endian, guest_index_mask_guest_endian);
              } else if (guest_index_endian == xenos::Endian::k16in32) {
                ReplaceResetIndex32To24<xenos::Endian::k16in32>(
                    host_indices, guest_indices, guest_draw_vertex_count,
                    guest_primitive_reset_index_guest_endian, guest_index_mask_guest_endian);
              } else {
                REX_UNHANDLED_CASE(guest_index_endian);
                return false;
              }
              cacheable.host_shader_index_endian =
                  full_32bit_vertex_indices_used_ ? guest_index_endian : xenos::Endian::kNone;
            }
            cache_transaction.SetNewResult(cacheable);
            cache_transaction.Commit();
          }
        }
      }

      // Backends like Vulkan without fullDrawIndexUint32 cannot fetch 32 bit DMA indices here.
      if (cacheable.index_buffer_type == ProcessedIndexBufferType::kGuestDMA &&
          guest_index_format == xenos::IndexFormat::kInt32 && !full_32bit_vertex_indices_used_ &&
          host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
        CacheTransaction cache_transaction(
            *this, CacheKey(guest_index_base, guest_draw_vertex_count, guest_index_format,
                            guest_index_endian, guest_primitive_reset_enabled,
                            xenos::PrimitiveType::kNone, true));
        if (cache_transaction.GetFoundResult()) {
          cacheable = *cache_transaction.GetFoundResult();
        } else {
          auto guest_indices = memory_.TranslatePhysical<const u32*>(guest_index_base);
          auto host_indices = reinterpret_cast<u32*>(RequestHostConvertedIndexBufferForCurrentFrame(
              xenos::IndexFormat::kInt32, guest_draw_vertex_count, true, guest_index_base,
              cacheable.host_index_buffer_handle));
          if (!host_indices) {
            return false;
          }
          for (u32 i = 0; i < guest_draw_vertex_count; ++i) {
            host_indices[i] =
                xenos::GPUSwap(guest_indices[i], guest_index_endian) & xenos::kVertexIndexMask;
          }
          cacheable.index_buffer_type = ProcessedIndexBufferType::kHostConverted;
          cacheable.host_shader_index_endian = xenos::Endian::kNone;
          cache_transaction.SetNewResult(cacheable);
          cache_transaction.Commit();
        }
      }
    }
  }

  if (cacheable.index_buffer_type == ProcessedIndexBufferType::kGuestDMA ||
      cacheable.index_buffer_type == ProcessedIndexBufferType::kHostBuiltinForDMA) {
    if (!shared_memory_.RequestRange(guest_index_base, guest_index_buffer_needed_bytes)) {
      REX_FAIL(gpu, false,
               "PrimitiveProcessor: Failed to request index buffer 0x{:08X}, 0x{:X} "
               "bytes needed, in the shared memory",
               guest_index_base, guest_index_buffer_needed_bytes);
    }
  }

  result_out.guest_primitive_type = guest_primitive_type;
  result_out.host_primitive_type = host_primitive_type;
  result_out.host_vertex_shader_type = host_vertex_shader_type;
  result_out.tessellation_mode = tessellation_mode;
  result_out.guest_draw_vertex_count = guest_draw_vertex_count;
  result_out.host_draw_vertex_count = cacheable.host_draw_vertex_count;
  result_out.line_loop_closing_index = line_loop_closing_index;
  result_out.index_buffer_type = cacheable.index_buffer_type;
  result_out.guest_index_base = guest_index_base;
  result_out.host_index_format = cacheable.host_index_format;
  result_out.host_shader_index_endian = cacheable.host_shader_index_endian;
  result_out.host_primitive_reset_enabled = cacheable.host_primitive_reset_enabled;
  result_out.host_index_buffer_handle = cacheable.host_index_buffer_handle;
  return true;
}

bool PrimitiveProcessor::IsResetUsed(const u16* source, u32 count, u16 reset_index_guest_endian) {
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    if (*(source++) == reset_index_guest_endian) {
      return true;
    }
  }
  if (count >= kSimdVectorU16Elements) {
    SimdVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSimdVectorU16Elements) {
      count -= kSimdVectorU16Elements;
      SimdVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSimdVectorU16Elements;
#if REX_ARCH_AMD64
      if (simde_mm_movemask_epi8(
              simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd))) {
        return true;
      }
#elif REX_ARCH_ARM64
      uint64x1_t is_any = vreinterpret_u64_u32(
          vqmovn_u64(vreinterpretq_u64_u16(vceqq_u16(source_simd, reset_index_guest_endian_simd))));
      if (vget_lane_u64(is_any, 0)) {
        return true;
      }
#else
#error SIMD 16-bit IsResetUsed not implemented.
#endif  // REX_ARCH_AMD64
    }
  }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count--) {
    if (*(source++) == reset_index_guest_endian) {
      return true;
    }
  }
  return false;
}

void PrimitiveProcessor::Get16BitResetIndexUsage(const u16* source, u32 count,
                                                 u16 reset_index_guest_endian,
                                                 bool& is_reset_index_used_out,
                                                 bool& is_ffff_used_as_vertex_index_out) {
  // Optimized for an unused reset index, so there are no early-outs for a simpler loop body.
  is_ffff_used_as_vertex_index_out = false;
  if (reset_index_guest_endian == UINT16_MAX) {
    is_reset_index_used_out = IsResetUsed(source, count, reset_index_guest_endian);
    return;
  }
  is_reset_index_used_out = false;
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    if (index == reset_index_guest_endian) {
      is_reset_index_used_out = true;
    }
    if (index == UINT16_MAX) {
      is_ffff_used_as_vertex_index_out = true;
    }
  }
  if (count >= kSimdVectorU16Elements) {
    SimdVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    SimdVectorU16 ffff_simd = ReplicateU16(UINT16_MAX);
    SimdVectorU16 is_reset_simd = ReplicateU16(0);
    SimdVectorU16 is_ffff_simd = ReplicateU16(0);
    while (count >= kSimdVectorU16Elements) {
      count -= kSimdVectorU16Elements;
      SimdVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSimdVectorU16Elements;
#if REX_ARCH_AMD64
      is_reset_simd = simde_mm_or_si128(
          is_reset_simd, simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd));
      is_ffff_simd = simde_mm_or_si128(is_ffff_simd, simde_mm_cmpeq_epi16(source_simd, ffff_simd));
#elif REX_ARCH_ARM64
      is_reset_simd =
          vorrq_u16(is_reset_simd, vceqq_u16(source_simd, reset_index_guest_endian_simd));
      is_ffff_simd = vmaxq_u16(is_ffff_simd, source_simd);
#else
#error SIMD Get16BitResetIndexUsage not implemented.
#endif  // REX_ARCH_AMD64
    }
#if REX_ARCH_AMD64
    if (simde_mm_movemask_epi8(is_reset_simd)) {
      is_reset_index_used_out = true;
    }
    if (simde_mm_movemask_epi8(is_ffff_simd)) {
      is_ffff_used_as_vertex_index_out = true;
    }
#elif REX_ARCH_ARM64
    uint64x1_t is_reset_any =
        vreinterpret_u64_u32(vqmovn_u64(vreinterpretq_u64_u16(is_reset_simd)));
    if (vget_lane_u64(is_reset_any, 0)) {
      is_reset_index_used_out = true;
    }
    uint64x1_t is_ffff_any =
        vreinterpret_u64_u32(vqmovn_u64(vreinterpretq_u64_u16(vceqq_u16(is_ffff_simd, ffff_simd))));
    if (vget_lane_u64(is_ffff_any, 0)) {
      is_ffff_used_as_vertex_index_out = true;
    }
#else
#error SIMD Get16BitResetIndexUsage not implemented.
#endif  // REX_ARCH_AMD64
  }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count--) {
    u16 index = *(source++);
    if (index == reset_index_guest_endian) {
      is_reset_index_used_out = true;
    }
    if (index == UINT16_MAX) {
      is_ffff_used_as_vertex_index_out = true;
    }
  }
}

bool PrimitiveProcessor::IsResetUsed(const u32* source, u32 count, u32 reset_index_guest_endian,
                                     u32 low_bits_mask_guest_endian) {
  // The Xenos only uses the low 24 bits of the index, so mask before comparing.
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    if ((*(source++) & low_bits_mask_guest_endian) == reset_index_guest_endian) {
      return true;
    }
  }
  if (count >= kSimdVectorU32Elements) {
    SimdVectorU32 reset_index_guest_endian_simd = ReplicateU32(reset_index_guest_endian);
    while (count >= kSimdVectorU32Elements) {
      count -= kSimdVectorU32Elements;
      SimdVectorU32 source_simd = LoadAlignedVectorU32(source);
      source += kSimdVectorU32Elements;
      SimdVectorU32 low_bits_mask_guest_endian_simd = ReplicateU32(low_bits_mask_guest_endian);
#if REX_ARCH_AMD64
      source_simd = simde_mm_and_si128(source_simd, low_bits_mask_guest_endian_simd);
      if (simde_mm_movemask_epi8(
              simde_mm_cmpeq_epi32(source_simd, reset_index_guest_endian_simd))) {
        return true;
      }
#elif REX_ARCH_ARM64
      source_simd = vandq_u32(source_simd, low_bits_mask_guest_endian_simd);
      uint64x1_t is_any = vreinterpret_u64_u32(
          vqmovn_u64(vreinterpretq_u64_u32(vceqq_u32(source_simd, reset_index_guest_endian_simd))));
      if (vget_lane_u64(is_any, 0)) {
        return true;
      }
#else
#error SIMD 32-bit IsResetUsed not implemented.
#endif  // REX_ARCH_AMD64
    }
  }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count--) {
    if ((*(source++) & low_bits_mask_guest_endian) == reset_index_guest_endian) {
      return true;
    }
  }
  return false;
}

void PrimitiveProcessor::ReplaceResetIndex16To16(u16* dest, const u16* source, u32 count,
                                                 u16 reset_index_guest_endian) {
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT16_MAX;
  }
  if (count >= kSimdVectorU16Elements) {
    SimdVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSimdVectorU16Elements) {
      count -= kSimdVectorU16Elements;
      // The comparison gives 0 or all ones, so the result is index | (index == reset_index).
      SimdVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSimdVectorU16Elements;
      SimdVectorU16 result_simd;
#if REX_ARCH_AMD64
      result_simd = simde_mm_or_si128(
          source_simd, simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd));
#elif REX_ARCH_ARM64
      result_simd = vorrq_u16(source_simd, vceqq_u16(source_simd, reset_index_guest_endian_simd));
#else
#error SIMD ReplaceResetIndex16To16 not implemented.
#endif  // REX_ARCH_AMD64
      StoreUnalignedVectorU16(dest, result_simd);
      dest += kSimdVectorU16Elements;
    }
  }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count--) {
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT16_MAX;
  }
}

void PrimitiveProcessor::ReplaceResetIndex16To24(u32* dest, const u16* source, u32 count,
                                                 u16 reset_index_guest_endian) {
#if REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count &&
         (reinterpret_cast<uintptr_t>(source) & (REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE - 1))) {
    --count;
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT32_MAX;
  }
  if (count >= kSimdVectorU16Elements) {
    SimdVectorU16 reset_index_guest_endian_simd = ReplicateU16(reset_index_guest_endian);
    while (count >= kSimdVectorU16Elements) {
      count -= kSimdVectorU16Elements;
      SimdVectorU16 source_simd = LoadAlignedVectorU16(source);
      source += kSimdVectorU16Elements;
#if REX_ARCH_AMD64
      simde__m128i are_reset = simde_mm_cmpeq_epi16(source_simd, reset_index_guest_endian_simd);
      simde__m128i result = simde_mm_or_si128(source_simd, are_reset);
      StoreUnalignedVectorU32(dest, simde_mm_unpacklo_epi16(result, are_reset));
      StoreUnalignedVectorU32(dest + kSimdVectorU16Elements / 2,
                              simde_mm_unpackhi_epi16(result, are_reset));
#elif REX_ARCH_ARM64
      uint16x8x2_t result;
      result.val[1] = vceqq_u16(source_simd, reset_index_guest_endian_simd);
      result.val[0] = vorrq_u16(source_simd, result.val[1]);
      vst2q_u16(reinterpret_cast<u16*>(dest), result);
#else
#error SIMD ReplaceResetIndex16To24 not implemented.
#endif  // REX_ARCH_AMD64
      dest += kSimdVectorU16Elements;
    }
  }
#endif  // REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE
  while (count--) {
    u16 index = *(source++);
    *(dest++) = index != reset_index_guest_endian ? index : UINT32_MAX;
  }
}

template void PrimitiveProcessor::ReplaceResetIndex32To24<xenos::Endian::kNone>(
    u32* dest, const u32* source, u32 count, u32 reset_index_guest_endian,
    u32 low_bits_mask_guest_endian);
template void PrimitiveProcessor::ReplaceResetIndex32To24<xenos::Endian::k8in16>(
    u32* dest, const u32* source, u32 count, u32 reset_index_guest_endian,
    u32 low_bits_mask_guest_endian);
template void PrimitiveProcessor::ReplaceResetIndex32To24<xenos::Endian::k8in32>(
    u32* dest, const u32* source, u32 count, u32 reset_index_guest_endian,
    u32 low_bits_mask_guest_endian);
template void PrimitiveProcessor::ReplaceResetIndex32To24<xenos::Endian::k16in32>(
    u32* dest, const u32* source, u32 count, u32 reset_index_guest_endian,
    u32 low_bits_mask_guest_endian);

#define REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION_NO_PASSTHROUGH(ConverterName) \
  template void PrimitiveProcessor::ConverterName(                                      \
      u32* dest, const u32* source, u32 source_index_count,                             \
      const To24NonSwappingIndexTransform& index_transform);                            \
  template void PrimitiveProcessor::ConverterName(                                      \
      u32* dest, const u32* source, u32 source_index_count,                             \
      const To24Swapping8In16IndexTransform& index_transform);                          \
  template void PrimitiveProcessor::ConverterName(                                      \
      u32* dest, const u32* source, u32 source_index_count,                             \
      const To24Swapping8In32IndexTransform& index_transform);                          \
  template void PrimitiveProcessor::ConverterName(                                      \
      u32* dest, const u32* source, u32 source_index_count,                             \
      const To24Swapping16In32IndexTransform& index_transform);
/**
 * Instantiates an index converter for 16 bit and 32 bit sources with every index transform.
 *
 * @param ConverterName Name of the PrimitiveProcessor converter member template to instantiate.
 *
 * @code
 * REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION(TriangleFanToList)
 * @endcode
 */
#define REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION(ConverterName) \
  template void PrimitiveProcessor::ConverterName(                       \
      u16* dest, const u16* source, u32 source_index_count,              \
      const PassthroughIndexTransform& index_transform);                 \
  template void PrimitiveProcessor::ConverterName(                       \
      u32* dest, const u32* source, u32 source_index_count,              \
      const PassthroughIndexTransform& index_transform);                 \
  REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION_NO_PASSTHROUGH(ConverterName)
REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION(TriangleFanToList)
REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION_NO_PASSTHROUGH(LineLoopToStrip)
REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION(QuadListToTriangleList)
#undef REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION_NO_PASSTHROUGH
#undef REXGPU_PRIMITIVE_PROCESSOR_INSTANTIATE_CONVERSION

void PrimitiveProcessor::LineLoopToStrip(
    u16* dest, const u16* source, u32 source_index_count,
    [[maybe_unused]] const PassthroughIndexTransform& index_transform) {
  if (source_index_count <= 1) {
    // To match GetLineLoopStripIndexCount.
    return;
  }
  std::memcpy(dest, source, sizeof(*source) * source_index_count);
  dest[source_index_count] = source[0];
}
void PrimitiveProcessor::LineLoopToStrip(
    u32* dest, const u32* source, u32 source_index_count,
    [[maybe_unused]] const PassthroughIndexTransform& index_transform) {
  if (source_index_count <= 1) {
    // To match GetLineLoopStripIndexCount.
    return;
  }
  std::memcpy(dest, source, sizeof(*source) * source_index_count);
  dest[source_index_count] = source[0];
}

template <std::invocable<u32> F>
u32 PrimitiveProcessor::GetMultiPrimitiveHostIndexCountAndRanges(
    const F& single_primitive_guest_to_host_count, const u16* source, u32 source_index_count,
    u16 reset_index_guest_endian, std::deque<SinglePrimitiveRange>& ranges_append_out) {
  u32 host_total_index_count = 0;
  u32 guest_index_offset = 0;
  for (;;) {
    u32 guest_primitive_offset = guest_index_offset;
    while (guest_index_offset < source_index_count) {
      if (source[guest_index_offset] == reset_index_guest_endian) {
        break;
      }
      ++guest_index_offset;
    }
    u32 guest_primitive_index_count = guest_index_offset - guest_primitive_offset;
    u32 host_primitive_index_count =
        single_primitive_guest_to_host_count(guest_primitive_index_count);
    if (host_primitive_index_count) {
      ranges_append_out.emplace_back(guest_primitive_offset, guest_primitive_index_count,
                                     host_primitive_index_count);
      host_total_index_count += host_primitive_index_count;
    }
    if (guest_index_offset >= source_index_count) {
      break;
    }
    ++guest_index_offset;
  }
  return host_total_index_count;
}

template <std::invocable<u32> F>
u32 PrimitiveProcessor::GetMultiPrimitiveHostIndexCountAndRanges(
    const F& single_primitive_guest_to_host_count, const u32* source, u32 source_index_count,
    u32 reset_index_guest_endian, u32 low_bits_mask_guest_endian,
    std::deque<SinglePrimitiveRange>& ranges_append_out) {
  u32 host_total_index_count = 0;
  u32 guest_index_offset = 0;
  for (;;) {
    u32 guest_primitive_offset = guest_index_offset;
    while (guest_index_offset < source_index_count) {
      if ((source[guest_index_offset] & low_bits_mask_guest_endian) == reset_index_guest_endian) {
        break;
      }
      ++guest_index_offset;
    }
    u32 guest_primitive_index_count = guest_index_offset - guest_primitive_offset;
    u32 host_primitive_index_count =
        single_primitive_guest_to_host_count(guest_primitive_index_count);
    if (host_primitive_index_count) {
      ranges_append_out.emplace_back(guest_primitive_offset, guest_primitive_index_count,
                                     host_primitive_index_count);
      host_total_index_count += host_primitive_index_count;
    }
    if (guest_index_offset >= source_index_count) {
      break;
    }
    ++guest_index_offset;
  }
  return host_total_index_count;
}

PrimitiveProcessor::CacheTransaction::CacheTransaction(PrimitiveProcessor& processor, CacheKey key)
    : processor_(processor), key_(key) {
  REX_ASSERT(processor_.cache_currently_processing_size_bytes_ == 0);
  if (REXCVAR_GET(::rex::gpu::cvars::primitive_processor_cache_min_indices) < 0 ||
      key_.count < u32(REXCVAR_GET(::rex::gpu::cvars::primitive_processor_cache_min_indices))) {
    key_.key = 0;
  }
  if (!key_.count) {
    return;
  }
  u32 size_bytes =
      (key_.format == xenos::IndexFormat::kInt16 ? sizeof(u16) : sizeof(u32)) * key_.count;
  {
    std::lock_guard<std::mutex> cache_lock(processor_.cache_mutex_);
    auto cache_map_it = processor_.cache_map_.find(key_);
    if (cache_map_it != processor_.cache_map_.end()) {
      result_ = processor_.cache_entry_pool_[cache_map_it->second].result;
      result_type_ = ResultType::kExisting;
    } else {
      // Inhibits storing the result if the range is modified during processing outside the lock.
      processor_.cache_currently_processing_base_ = key_.base;
      processor_.cache_currently_processing_size_bytes_ = size_bytes;
    }
  }
  if (result_type_ != ResultType::kExisting) {
    // Enable the invalidation callback before reading indices, only when processing is needed.
    if (!processor_.memory_invalidation_callback_handle_) {
      processor_.memory_invalidation_callback_handle_ =
          processor_.memory_.RegisterPhysicalMemoryInvalidationCallback(
              MemoryInvalidationCallbackThunk, &processor_);
    }
    processor_.memory_.EnablePhysicalMemoryAccessCallbacks(key_.base, size_bytes, true, false);
  }
}

void PrimitiveProcessor::CacheTransaction::Commit() {
  if (committed_ || !key_.count || result_type_ == ResultType::kExisting) {
    return;
  }

  std::lock_guard<std::mutex> cache_lock(processor_.cache_mutex_);

  processor_.cache_currently_processing_base_ = 0;
  processor_.cache_currently_processing_size_bytes_ = 0;
  committed_ = true;

  if (result_type_ == ResultType::kNewSet) {
    const bool from_free_list = processor_.cache_bucket_free_first_entry_ != SIZE_MAX;
    size_t new_entry_index;
    if (from_free_list) {
      new_entry_index = processor_.cache_bucket_free_first_entry_;
    } else {
      new_entry_index = processor_.cache_entry_pool_.size();
      processor_.cache_entry_pool_.emplace_back();
    }
    processor_.cache_map_.emplace(key_, new_entry_index);
    if (from_free_list) {
      processor_.cache_bucket_free_first_entry_ =
          processor_.cache_entry_pool_[new_entry_index].free_next;
    }
    CacheEntry& new_entry = processor_.cache_entry_pool_[new_entry_index];

    u32 bucket_start_index = key_.base >> kCacheBucketSizeBytesLog2;
    u32 bucket_count = CacheEntry::GetBucketCount(key_);
    for (u32 link_index = 0; link_index < bucket_count; ++link_index) {
      new_entry.buckets_prev[link_index] = SIZE_MAX;
      u32 bucket_index = bucket_start_index + link_index;
      u64& bucket_non_empty_l1_ref = processor_.cache_buckets_non_empty_l1_[bucket_index >> 6];
      u64 bucket_non_empty_l1_bit = static_cast<u64>(1) << (bucket_index & 63);
      size_t& bucket_first_entry_ref = processor_.cache_bucket_first_entries_[bucket_index];
      if (bucket_non_empty_l1_ref & bucket_non_empty_l1_bit) {
        new_entry.buckets_next[link_index] = bucket_first_entry_ref;
        CacheEntry& bucket_first_entry = processor_.cache_entry_pool_[bucket_first_entry_ref];
        // An entry spans at most 2 buckets, so bucket_index is its [0] bucket or its [1].
        bucket_first_entry.buckets_prev[size_t(
            (bucket_first_entry.key.base >> kCacheBucketSizeBytesLog2) != bucket_index)] =
            new_entry_index;
      } else {
        new_entry.buckets_next[link_index] = SIZE_MAX;
        bucket_non_empty_l1_ref |= bucket_non_empty_l1_bit;
        processor_.UpdateCacheBucketsNonEmptyL2(bucket_index >> 6, cache_lock);
      }
      bucket_first_entry_ref = new_entry_index;
    }

    new_entry.key = key_;
    new_entry.result = result_;
  }
}

PrimitiveProcessor::CacheTransaction::~CacheTransaction() {
  if (committed_ || !key_.count || result_type_ == ResultType::kExisting) {
    return;
  }
  std::lock_guard<std::mutex> cache_lock(processor_.cache_mutex_);
  processor_.cache_currently_processing_base_ = 0;
  processor_.cache_currently_processing_size_bytes_ = 0;
}

std::pair<u32, u32> PrimitiveProcessor::MemoryInvalidationCallback(u32 physical_address_start,
                                                                   u32 length, bool exact_range) {
  if (length == 0 || physical_address_start >= SharedMemory::kBufferSize) {
    return std::make_pair(static_cast<u32>(0), UINT32_MAX);
  }
  length = std::min(length, SharedMemory::kBufferSize - physical_address_start);
  u32 physical_address_end = physical_address_start + length;
  if (!exact_range) {
    // Invalidate whole buckets for access callbacks, which cuts access violation handling.
    physical_address_start &= ~(kCacheBucketSizeBytes - 1);
    physical_address_end = rex::Align(physical_address_end, kCacheBucketSizeBytes);
  }
  bool any_invalidated = false;
  u32 bucket_index_first = physical_address_start >> kCacheBucketSizeBytesLog2;
  u32 bucket_index_last = (physical_address_end - 1) >> kCacheBucketSizeBytesLog2;
  u32 bucket_l1_bits_index_first = bucket_index_first >> 6;
  u32 bucket_l1_bits_index_last = bucket_index_last >> 6;
  u32 bucket_l2_bits_index_first = bucket_index_first >> 12;
  u32 bucket_l2_bits_index_last = bucket_index_last >> 12;
  std::lock_guard<std::mutex> cache_lock(cache_mutex_);
  for (u32 bucket_l2_bits_index = bucket_l2_bits_index_first;
       bucket_l2_bits_index <= bucket_l2_bits_index_last; ++bucket_l2_bits_index) {
    u64 bucket_l2_bits_mask = UINT64_MAX;
    if (bucket_l2_bits_index == bucket_l2_bits_index_first) {
      bucket_l2_bits_mask &= ~((static_cast<u64>(1) << (bucket_l1_bits_index_first & 63)) - 1);
    }
    if (bucket_l2_bits_index == bucket_l2_bits_index_last &&
        (bucket_l1_bits_index_last & 63) != 63) {
      bucket_l2_bits_mask &= (static_cast<u64>(1) << ((bucket_l1_bits_index_last & 63) + 1)) - 1;
    }
    // L2 bits are not cached, as unlinking may modify them.
    while (bucket_l2_bits_mask) {
      u32 bucket_l2_bit_shift;
      if (!rex::bit_scan_forward(
              cache_buckets_non_empty_l2_[bucket_l2_bits_index] & bucket_l2_bits_mask,
              &bucket_l2_bit_shift)) {
        break;
      }
      bucket_l2_bits_mask &= ~(static_cast<u64>(1) << bucket_l2_bit_shift);
      u32 bucket_l1_bits_index = (bucket_l2_bits_index << 6) | bucket_l2_bit_shift;
      u64 bucket_l1_bits_mask = UINT64_MAX;
      if (bucket_l1_bits_index == bucket_l1_bits_index_first) {
        bucket_l1_bits_mask &= ~((static_cast<u64>(1) << (bucket_index_first & 63)) - 1);
      }
      if (bucket_l1_bits_index == bucket_l1_bits_index_last && (bucket_index_last & 63) != 63) {
        bucket_l1_bits_mask &= (static_cast<u64>(1) << ((bucket_index_last & 63) + 1)) - 1;
      }
      // L1 bits are not cached, as unlinking may modify them.
      while (bucket_l1_bits_mask) {
        u32 bucket_l1_bit_shift;
        if (!rex::bit_scan_forward(
                cache_buckets_non_empty_l1_[bucket_l1_bits_index] & bucket_l1_bits_mask,
                &bucket_l1_bit_shift)) {
          break;
        }
        bucket_l1_bits_mask &= ~(static_cast<u64>(1) << bucket_l1_bit_shift);
        u32 bucket_index = (bucket_l1_bits_index << 6) | bucket_l1_bit_shift;
        size_t entry_index = cache_bucket_first_entries_[bucket_index];
        do {
          CacheEntry& entry = cache_entry_pool_[entry_index];
          CacheKey entry_key = entry.key;
          // An entry spans at most 2 buckets, so bucket_index is its [0] bucket or its [1].
          u32 entry_bucket_index_first = entry_key.base >> kCacheBucketSizeBytesLog2;
          REX_ASSERT((bucket_index - entry_bucket_index_first) <= 1,
                     "Cache entries only store list links within two buckets");
          size_t next_entry_index = entry.buckets_next[bucket_index - entry_bucket_index_first];
          // For exact_range, entries outside the range are not invalidated.
          if (entry_key.base < physical_address_end) {
            u32 entry_end = entry_key.base + entry_key.GetSizeBytes();
            if (entry_end > physical_address_end) {
              any_invalidated = true;
              auto entry_map_it = cache_map_.find(entry_key);
              REX_ASSERT(entry_map_it != cache_map_.end());
              if (entry_map_it != cache_map_.end()) {
                cache_map_.erase(entry_map_it);
              }
              u32 entry_link_index_last =
                  ((entry_end - 1) >> kCacheBucketSizeBytesLog2) - entry_bucket_index_first;
              REX_ASSERT(entry_link_index_last <= 1,
                         "Cache entries only store list links within two buckets");
              for (u32 entry_link_index = 0; entry_link_index <= entry_link_index_last;
                   ++entry_link_index) {
                u32 entry_bucket_index = entry_bucket_index_first + entry_link_index;
                size_t entry_link_prev = entry.buckets_prev[entry_link_index];
                size_t entry_link_next = entry.buckets_next[entry_link_index];
                if (entry_link_prev != SIZE_MAX) {
                  CacheEntry& entry_prev = cache_entry_pool_[entry_link_prev];
                  entry_prev.buckets_next[static_cast<size_t>(
                      (entry_prev.key.base >> kCacheBucketSizeBytesLog2) != entry_bucket_index)] =
                      entry_link_next;
                } else {
                  if (entry_link_next != SIZE_MAX) {
                    cache_bucket_first_entries_[entry_bucket_index] = entry_link_next;
                  } else {
                    cache_buckets_non_empty_l1_[entry_bucket_index >> 6] &=
                        ~(static_cast<u64>(1) << (entry_bucket_index & 63));
                    UpdateCacheBucketsNonEmptyL2(entry_bucket_index >> 6, cache_lock);
                  }
                }
                if (entry_link_next != SIZE_MAX) {
                  CacheEntry& entry_next = cache_entry_pool_[entry_link_next];
                  entry_next.buckets_prev[static_cast<size_t>(
                      (entry_next.key.base >> kCacheBucketSizeBytesLog2) != entry_bucket_index)] =
                      entry_link_prev;
                }
              }
              entry.free_next = cache_bucket_free_first_entry_;
              cache_bucket_free_first_entry_ = entry_index;
            }
          }
          entry_index = next_entry_index;
        } while (entry_index != SIZE_MAX);
      }
    }
  }
  return any_invalidated
             ? std::make_pair(physical_address_start, physical_address_end - physical_address_start)
             : std::make_pair(static_cast<u32>(0), UINT32_MAX);
}

std::pair<u32, u32> PrimitiveProcessor::MemoryInvalidationCallbackThunk(void* context_ptr,
                                                                        u32 physical_address_start,
                                                                        u32 length,
                                                                        bool exact_range) {
  return reinterpret_cast<PrimitiveProcessor*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

}  // namespace rex::gpu
