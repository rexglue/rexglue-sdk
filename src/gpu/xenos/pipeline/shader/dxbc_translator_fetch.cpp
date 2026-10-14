/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

#include <format>

#include <rex/assert.h>
#include <rex/cvar.h>
#include "gpu/xenos/pipeline/render_target/cache.h"
#include "gpu/xenos/pipeline/shader/dxbc_translator.h"
#include <rex/math.h>
#include <rex/string.h>

#include "gpu/xenos/cvar_table.h"

namespace rex::gpu {
using namespace xenos::ucode;

/**
 * Translates a vertex fetch instruction.
 *
 * The address part `base + index * stride` is kept in system_temp_grad_v_vfetch_address_.w and
 * shared by a vfetch_full and its vfetch_mini instructions.
 *
 * @param instr Parsed vertex fetch instruction.
 */
void DXBCShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }
  UpdateInstructionPredicationAndEmitDisassembly(instr.is_predicated, instr.predicate_condition);

  u32 used_result_components = instr.result.GetUsedResultComponents();
  u32 needed_words =
      xenos::GetVertexFormatNeededWords(instr.attributes.data_format, used_result_components);
  // A vfetch_full address may be needed by a vfetch_mini, so it is calculated before exiting.
  if (!needed_words && instr.is_mini_fetch) {
    // Nothing to load, so the unpacking below can assume at least 1 needed word.
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  // vf0 is in [0].xy of the fetch constants array, vf1 in [0].zw and vf2 in [1].xy.
  if (cbuffer_index_fetch_constants_ == kBindingIndexUnallocated) {
    cbuffer_index_fetch_constants_ = cbuffer_count_++;
  }
  dxbc::Src fetch_constant_src(dxbc::Src::CB(
      cbuffer_index_fetch_constants_, std::to_underlying(CbufferRegister::kFetchConstants),
      instr.operands[1].storage_index >> 1,
      (instr.operands[1].storage_index & 1) ? 0b10101110 : 0b00000100));

  // A vfetch_mini reuses the address part of its vfetch_full, the index GPR may be overwritten.

  dxbc::Src address_src(dxbc::Src::R(system_temp_grad_v_vfetch_address_, dxbc::Src::kWWWW));
  if (!instr.is_mini_fetch) {
    dxbc::Dest address_dest(dxbc::Dest::R(system_temp_grad_v_vfetch_address_, 0b1000));
    if (instr.attributes.stride) {
      // Rounding uses floor(index + 0.5) since round to nearest even makes no sense for addressing.
      {
        bool index_operand_temp_pushed = false;
        dxbc::Src index_operand(LoadOperand(instr.operands[0], 0b0001, index_operand_temp_pushed)
                                    .SelectFromSwizzled(0));
        if (instr.attributes.is_index_rounded) {
          a_.OpAdd(address_dest, index_operand, dxbc::Src::LF(0.5f));
          a_.OpRoundNI(address_dest, address_src);
        } else {
          a_.OpRoundNI(address_dest, index_operand);
        }
        if (index_operand_temp_pushed) {
          PopSystemTemp();
        }
      }
      a_.OpFToI(address_dest, address_src);
      a_.OpAnd(dxbc::Dest::R(system_temp_result_, 0b1000), fetch_constant_src.SelectFromSwizzled(0),
               dxbc::Src::LU(~static_cast<u32>(3)));
      a_.OpIMAd(address_dest, address_src, dxbc::Src::LU(instr.attributes.stride * sizeof(u32)),
                dxbc::Src::R(system_temp_result_, dxbc::Src::kWWWW));
    } else {
      a_.OpAnd(address_dest, fetch_constant_src.SelectFromSwizzled(0),
               dxbc::Src::LU(~static_cast<u32>(3)));
    }
  }

  if (!needed_words) {
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  dxbc::Dest address_temp_dest(dxbc::Dest::R(system_temp_result_, 0b1000));
  dxbc::Src address_temp_src(dxbc::Src::R(system_temp_result_, dxbc::Src::kWWWW));

  // Further offsets go to system_temp_result_.w so a vfetch_full and its minis share the address.

  u32 first_word_index;
  rex::bit_scan_forward(needed_words, &first_word_index);
  i32 first_word_buffer_offset = instr.attributes.offset + static_cast<i32>(first_word_index);
  if (first_word_buffer_offset) {
    a_.OpIAdd(address_temp_dest, address_src,
              dxbc::Src::LI(first_word_buffer_offset * static_cast<i32>(sizeof(u32))));
    address_src = address_temp_src;
  }

  // Bounds are not checked, and Adreno 200 returns 0 for words out of bounds.

  // Loading as FXC does (Load4.xyw is Load2 and Load) compromises between AMD and Nvidia buffers.

  // Shared memory is an SRV or, with memexport, a UAV, so the likelier SRV case goes first.
  a_.OpAnd(dxbc::Dest::R(system_temp_result_, 0b0001), LoadFlagsSystemConstant(),
           dxbc::Src::LU(kSysFlagSharedMemoryIsUAV));
  a_.OpIf(false, dxbc::Src::R(system_temp_result_, dxbc::Src::kXXXX));
  if (srv_index_shared_memory_ == kBindingIndexUnallocated) {
    srv_index_shared_memory_ = srv_count_++;
  }
  if (uav_index_shared_memory_ == kBindingIndexUnallocated) {
    uav_index_shared_memory_ = uav_count_++;
  }
  for (u32 i = 0; i < 2; ++i) {
    if (i) {
      a_.OpElse();
    }
    dxbc::Src shared_memory_src(
        i ? dxbc::Src::U(uav_index_shared_memory_, std::to_underlying(UAVRegister::kSharedMemory))
          : dxbc::Src::T(srv_index_shared_memory_,
                         std::to_underlying(SRVMainRegister::kSharedMemory)));
    u32 needed_words_remaining = needed_words;
    u32 word_index_previous = first_word_index;
    while (needed_words_remaining) {
      u32 word_index;
      rex::bit_scan_forward(needed_words_remaining, &word_index);
      u32 word_count;
      rex::bit_scan_forward(~(needed_words_remaining >> word_index), &word_count);
      needed_words_remaining &=
          ~((static_cast<u32>(1) << (word_index + word_count)) - static_cast<u32>(1));
      if (word_index != word_index_previous) {
        a_.OpIAdd(address_temp_dest, address_src,
                  dxbc::Src::LU((word_index - word_index_previous) * sizeof(u32)));
        address_src = address_temp_src;
        word_index_previous = word_index;
      }
      // ld_raw can write the first components of a multi component result or any 1 component.
      dxbc::Dest words_result_dest(
          dxbc::Dest::R(system_temp_result_, ((1 << word_count) - 1) << word_index));
      if (!word_index || word_count == 1) {
        a_.OpLdRaw(words_result_dest, address_src, shared_memory_src);
      } else {
        u32 load_temp = PushSystemTemp();
        a_.OpLdRaw(dxbc::Dest::R(load_temp, (1 << word_count) - 1), address_src, shared_memory_src);
        a_.OpMov(words_result_dest,
                 dxbc::Src::R(load_temp, (dxbc::Src::kXYZW & ((1 << (word_count * 2)) - 1))
                                             << (word_index * 2)));
        PopSystemTemp();
      }
    }
  }
  a_.OpEndIf();

  dxbc::Src result_src(dxbc::Src::R(system_temp_result_));

  {
    u32 swap_temp = PushSystemTemp();

    u32 endian_temp, endian_temp_component;
    if (needed_words == 0b1111) {
      endian_temp = PushSystemTemp();
      endian_temp_component = 0;
    } else {
      endian_temp = swap_temp;
      rex::bit_scan_forward(~needed_words, &endian_temp_component);
    }
    a_.OpAnd(dxbc::Dest::R(endian_temp, 1 << endian_temp_component),
             fetch_constant_src.SelectFromSwizzled(1), dxbc::Src::LU(0b11));
    dxbc::Src endian_src(dxbc::Src::R(endian_temp).Select(endian_temp_component));

    dxbc::Dest swap_temp_dest(dxbc::Dest::R(swap_temp, needed_words));
    dxbc::Src swap_temp_src(dxbc::Src::R(swap_temp));
    dxbc::Dest swap_result_dest(dxbc::Dest::R(system_temp_result_, needed_words));

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in16)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)));
    a_.OpAnd(swap_temp_dest, result_src, dxbc::Src::LU(0x00FF00FF));
    a_.OpUShR(swap_result_dest, result_src, dxbc::Src::LU(8));
    a_.OpAnd(swap_result_dest, result_src, dxbc::Src::LU(0x00FF00FF));
    a_.OpUMAd(swap_result_dest, swap_temp_src, dxbc::Src::LU(256), result_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    a_.OpSwitch(endian_src);
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k8in32)));
    a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::Endian128::k16in32)));
    a_.OpUShR(swap_temp_dest, result_src, dxbc::Src::LU(16));
    a_.OpBFI(swap_result_dest, dxbc::Src::LU(16), dxbc::Src::LU(16), result_src, swap_temp_src);
    a_.OpBreak();
    a_.OpEndSwitch();

    PopSystemTemp((endian_temp != swap_temp) ? 2 : 1);
  }

  u32 used_format_components =
      used_result_components &
      ((1 << xenos::GetVertexFormatComponentCount(instr.attributes.data_format)) - 1);
  dxbc::Dest result_unpacked_dest(dxbc::Dest::R(system_temp_result_, used_format_components));
  REX_ASSERT(used_format_components != 0);
  u32 packed_widths[4] = {}, packed_offsets[4] = {};
  u32 packed_swizzle = dxbc::Src::kXXXX;
  switch (instr.attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 8;
      packed_offsets[1] = 8;
      packed_offsets[2] = 16;
      packed_offsets[3] = 24;
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      packed_widths[0] = packed_widths[1] = packed_widths[2] = 10;
      packed_widths[3] = 2;
      packed_offsets[1] = 10;
      packed_offsets[2] = 20;
      packed_offsets[3] = 30;
      break;
    case xenos::VertexFormat::k_10_11_11:
      packed_widths[0] = packed_widths[1] = 11;
      packed_widths[2] = 10;
      packed_offsets[1] = 11;
      packed_offsets[2] = 22;
      break;
    case xenos::VertexFormat::k_11_11_10:
      packed_widths[0] = 10;
      packed_widths[1] = packed_widths[2] = 11;
      packed_offsets[1] = 10;
      packed_offsets[2] = 21;
      break;
    case xenos::VertexFormat::k_16_16:
      packed_widths[0] = packed_widths[1] = 16;
      packed_offsets[1] = 16;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 16;
      packed_offsets[1] = packed_offsets[3] = 16;
      packed_swizzle = 0b01010000;
      break;
    default:
      break;
  }
  if (packed_widths[0]) {
    if (instr.attributes.is_signed) {
      a_.OpIBFE(result_unpacked_dest, dxbc::Src::LP(packed_widths), dxbc::Src::LP(packed_offsets),
                dxbc::Src::R(system_temp_result_, packed_swizzle));
      a_.OpIToF(result_unpacked_dest, result_src);
      if (!instr.attributes.is_integer) {
        f32 packed_scales[4] = {};
        switch (instr.attributes.signed_rf_mode) {
          case xenos::SignedRepeatingFractionMode::kZeroClampMinusOne: {
            u32 packed_scales_mask = 0b0000;
            for (u32 i = 0; i < 4; ++i) {
              if (!(used_format_components & (1 << i))) {
                continue;
              }
              if (packed_widths[i] > 2) {
                packed_scales[i] =
                    1.0f / static_cast<f32>((static_cast<u32>(1) << (packed_widths[i] - 1)) - 1);
                packed_scales_mask |= 1 << i;
              }
            }
            if (packed_scales_mask) {
              a_.OpMul(dxbc::Dest::R(system_temp_result_, packed_scales_mask), result_src,
                       dxbc::Src::LP(packed_scales));
            }
            // Both -(2^(n-1)) and -(2^(n-1)-1) are treated as -1.
            a_.OpMax(result_unpacked_dest, result_src, dxbc::Src::LF(-1.0f));
          } break;
          case xenos::SignedRepeatingFractionMode::kNoZero: {
            f32 packed_zeros[4] = {};
            for (u32 i = 0; i < 4; ++i) {
              if (!(used_format_components & (1 << i))) {
                continue;
              }
              REX_ASSERT(packed_widths[i] != 0);
              packed_zeros[i] =
                  1.0f / static_cast<f32>((static_cast<u32>(1) << packed_widths[i]) - 1);
              packed_scales[i] = 2.0f * packed_zeros[i];
            }
            a_.OpMAd(result_unpacked_dest, result_src, dxbc::Src::LP(packed_scales),
                     dxbc::Src::LP(packed_zeros));
          } break;
          default:
            REX_UNHANDLED_CASE(instr.attributes.signed_rf_mode);
        }
      }
    } else {
      a_.OpUBFE(result_unpacked_dest, dxbc::Src::LP(packed_widths), dxbc::Src::LP(packed_offsets),
                dxbc::Src::R(system_temp_result_, packed_swizzle));
      a_.OpUToF(result_unpacked_dest, result_src);
      if (!instr.attributes.is_integer) {
        f32 packed_scales[4] = {};
        u32 packed_scales_mask = 0b0000;
        for (u32 i = 0; i < 4; ++i) {
          if (!(used_format_components & (1 << i))) {
            continue;
          }
          if (packed_widths[i] > 1) {
            packed_scales[i] =
                1.0f / static_cast<f32>((static_cast<u32>(1) << packed_widths[i]) - 1);
            packed_scales_mask |= 1 << i;
          }
        }
        if (packed_scales_mask) {
          a_.OpMul(dxbc::Dest::R(system_temp_result_, packed_scales_mask), result_src,
                   dxbc::Src::LP(packed_scales));
        }
      }
    }
  } else {
    switch (instr.attributes.data_format) {
      case xenos::VertexFormat::k_16_16_FLOAT:
      case xenos::VertexFormat::k_16_16_16_16_FLOAT:
        // Converts from Direct3D 10+ float16 with NaNs, not Xenos float16 with extended range.
        a_.OpUBFE(result_unpacked_dest, dxbc::Src::LU(16), dxbc::Src::LU(0, 16, 0, 16),
                  dxbc::Src::R(system_temp_result_, 0b01010000));
        a_.OpF16ToF32(result_unpacked_dest, result_src);
        break;
      case xenos::VertexFormat::k_32:
      case xenos::VertexFormat::k_32_32:
      case xenos::VertexFormat::k_32_32_32_32:
        if (instr.attributes.is_signed) {
          a_.OpIToF(result_unpacked_dest, result_src);
        } else {
          a_.OpUToF(result_unpacked_dest, result_src);
        }
        if (!instr.attributes.is_integer) {
          if (instr.attributes.is_signed) {
            switch (instr.attributes.signed_rf_mode) {
              case xenos::SignedRepeatingFractionMode::kZeroClampMinusOne:
                a_.OpMul(result_unpacked_dest, result_src, dxbc::Src::LF(1.0f / 2147483647.0f));
                // No clamp to -1 for signed since 1/(2^31-1) rounds to 1/(2^31) as float32.
                break;
              case xenos::SignedRepeatingFractionMode::kNoZero:
                a_.OpMAd(result_unpacked_dest, result_src, dxbc::Src::LF(1.0f / 2147483647.5f),
                         dxbc::Src::LF(0.5f / 2147483647.5f));
                break;
              default:
                REX_UNHANDLED_CASE(instr.attributes.signed_rf_mode);
            }
          } else {
            a_.OpMul(result_unpacked_dest, result_src, dxbc::Src::LF(1.0f / 4294967295.0f));
          }
        }
        break;
      case xenos::VertexFormat::k_32_FLOAT:
      case xenos::VertexFormat::k_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      case xenos::VertexFormat::k_32_32_32_FLOAT:
        break;
      default:
        REX_ASSERT(packed_widths[0] != 0);
        break;
    }
  }

  if (instr.attributes.exp_adjust) {
    a_.OpMul(result_unpacked_dest, result_src,
             dxbc::Src::LF(std::ldexp(1.0f, instr.attributes.exp_adjust)));
  }

  u32 used_missing_components = used_result_components & ~used_format_components;
  if (used_missing_components) {
    a_.OpMov(dxbc::Dest::R(system_temp_result_, used_missing_components), dxbc::Src::LF(0.0f));
  }

  StoreResult(instr.result, dxbc::Src::R(system_temp_result_));
}

u32 DXBCShaderTranslator::FindOrAddTextureBinding(u32 fetch_constant,
                                                  xenos::FetchOpDimension dimension,
                                                  bool is_signed) {
  // 1D and 2D textures, stacked ones included, are treated as 2D arrays.
  if (dimension == xenos::FetchOpDimension::k1D) {
    dimension = xenos::FetchOpDimension::k2D;
  }
  for (u32 i = 0; i < u32(texture_bindings_.size()); ++i) {
    const TextureBinding& texture_binding = texture_bindings_[i];
    if (texture_binding.fetch_constant == fetch_constant &&
        texture_binding.dimension == dimension && texture_binding.is_signed == is_signed) {
      return i;
    }
  }
  if (texture_bindings_.size() >= kMaxTextureBindings) {
    REX_ASSERT_ALWAYS();
    return kMaxTextureBindings - 1;
  }
  u32 texture_binding_index = static_cast<u32>(texture_bindings_.size());
  TextureBinding& new_texture_binding = texture_bindings_.emplace_back();
  if (!bindless_resources_used_) {
    new_texture_binding.bindful_srv_index = srv_count_++;
    texture_bindings_for_bindful_srv_indices_.insert(
        {new_texture_binding.bindful_srv_index, texture_binding_index});
    const char* dimension_name;
    switch (dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        dimension_name = "3d";
        break;
      case xenos::FetchOpDimension::kCube:
        dimension_name = "cube";
        break;
      default:
        dimension_name = "2d";
    }
    new_texture_binding.bindful_name =
        std::format("xe_texture{}_{}_{}", fetch_constant, dimension_name, is_signed ? 's' : 'u');
  } else {
    new_texture_binding.bindful_srv_index = kBindingIndexUnallocated;
  }
  new_texture_binding.bindful_srv_rdef_name_ptr = 0;
  // 0 if not bindless so it is consistent for hashing.
  new_texture_binding.bindless_descriptor_index =
      bindless_resources_used_ ? GetBindlessResourceCount() : 0;
  new_texture_binding.fetch_constant = fetch_constant;
  new_texture_binding.dimension = dimension;
  new_texture_binding.is_signed = is_signed;
  return texture_binding_index;
}

u32 DXBCShaderTranslator::FindOrAddSamplerBinding(u32 fetch_constant,
                                                  xenos::TextureFilter mag_filter,
                                                  xenos::TextureFilter min_filter,
                                                  xenos::TextureFilter mip_filter,
                                                  xenos::AnisoFilter aniso_filter) {
  // Anisotropic filtering implies linear filtering in Direct3D 12.
  if (aniso_filter != xenos::AnisoFilter::kDisabled &&
      aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    mag_filter = xenos::TextureFilter::kLinear;
    min_filter = xenos::TextureFilter::kLinear;
    mip_filter = xenos::TextureFilter::kLinear;
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax161);
  }
  for (u32 i = 0; i < u32(sampler_bindings_.size()); ++i) {
    const SamplerBinding& sampler_binding = sampler_bindings_[i];
    if (sampler_binding.fetch_constant == fetch_constant &&
        sampler_binding.mag_filter == mag_filter && sampler_binding.min_filter == min_filter &&
        sampler_binding.mip_filter == mip_filter && sampler_binding.aniso_filter == aniso_filter) {
      return i;
    }
  }
  if (sampler_bindings_.size() >= kMaxSamplerBindings) {
    REX_ASSERT_ALWAYS();
    return kMaxSamplerBindings - 1;
  }
  SamplerBinding& new_sampler_binding = sampler_bindings_.emplace_back();
  // 0 if not bindless so it is consistent for hashing.
  new_sampler_binding.bindless_descriptor_index =
      bindless_resources_used_ ? GetBindlessResourceCount() : 0;
  new_sampler_binding.fetch_constant = fetch_constant;
  new_sampler_binding.mag_filter = mag_filter;
  new_sampler_binding.min_filter = min_filter;
  new_sampler_binding.mip_filter = mip_filter;
  new_sampler_binding.aniso_filter = aniso_filter;
  if (!bindless_resources_used_) {
    std::ostringstream name;
    name << "xe_sampler" << fetch_constant;
    if (aniso_filter == xenos::AnisoFilter::kDisabled ||
        aniso_filter == xenos::AnisoFilter::kUseFetchConst) {
      static const char kFilterSuffixes[] = {'p', 'l', 'b', 'f'};
      name << '_' << kFilterSuffixes[static_cast<u32>(mag_filter)]
           << kFilterSuffixes[static_cast<u32>(min_filter)]
           << kFilterSuffixes[static_cast<u32>(mip_filter)];
    }
    if (aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
      if (aniso_filter == xenos::AnisoFilter::kDisabled) {
        name << "_a0";
      } else {
        name << "_a" << (UINT32_C(1) << (static_cast<u32>(aniso_filter) - 1));
      }
    }
    new_sampler_binding.bindful_name = name.str();
  }
  return static_cast<u32>(sampler_bindings_.size() - 1);
}

/**
 * Translates a texture fetch instruction.
 *
 * The texture size is loaded as X width for 1D, X width and Y height for 2D and cube, and X width,
 * Y height, Z depth and W for 3D, where W is 0 for a stacked 2D texture and 1 for 3D.
 *
 * When allocated, srv_selection_temp holds the following.
 * - X is whether all components are signed, with a fallback for a single component. When false the
 * unsigned SRV is sampled.
 * - Y is whether any component is signed, with a fallback for a single component. When true the
 * signed SRV is sampled.
 * - Z is the lerp factor between 2 layers of a stacked texture not forced to point sampling, with
 * an l(0.0) fallback.
 * - W is scratch for calculations, then the unsigned or signed SRV description index.
 *
 * @param instr Parsed texture fetch instruction.
 */
void DXBCShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  if (emit_source_map_) {
    instruction_disassembly_buffer_.Reset();
    instr.Disassemble(&instruction_disassembly_buffer_);
  }
  UpdateInstructionPredicationAndEmitDisassembly(instr.is_predicated, instr.predicate_condition);

  switch (instr.opcode) {
    case FetchOpcode::kSetTextureLOD: {
      bool lod_operand_temp_pushed = false;
      a_.OpMov(
          dxbc::Dest::R(system_temp_grad_h_lod_, 0b1000),
          LoadOperand(instr.operands[0], 0b0001, lod_operand_temp_pushed).SelectFromSwizzled(0));
      if (lod_operand_temp_pushed) {
        PopSystemTemp();
      }
      return;
    }
    case FetchOpcode::kSetTextureGradientsHorz: {
      bool grad_operand_temp_pushed = false;
      a_.OpMov(dxbc::Dest::R(system_temp_grad_h_lod_, 0b0111),
               LoadOperand(instr.operands[0], 0b0111, grad_operand_temp_pushed));
      if (grad_operand_temp_pushed) {
        PopSystemTemp();
      }
      return;
    }
    case FetchOpcode::kSetTextureGradientsVert: {
      bool grad_operand_temp_pushed = false;
      a_.OpMov(dxbc::Dest::R(system_temp_grad_v_vfetch_address_, 0b0111),
               LoadOperand(instr.operands[0], 0b0111, grad_operand_temp_pushed));
      if (grad_operand_temp_pushed) {
        PopSystemTemp();
      }
      return;
    }
    default:
      break;
  }

  u32 used_result_components = instr.result.GetUsedResultComponents();
  u32 used_result_nonzero_components = instr.GetNonZeroResultComponents();
  // The LOD is not applied in getWeights, which is mostly used for PCF of shadow maps without mips.
  if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    used_result_nonzero_components &= ~static_cast<u32>(0b1000);
  }
  if (!used_result_nonzero_components) {
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  if (instr.opcode == FetchOpcode::kGetTextureGradients) {
    bool grad_operand_temp_pushed = false;
    dxbc::Src grad_operand =
        LoadOperand(instr.operands[0],
                    ((used_result_nonzero_components & 0b0011) ? 0b0001 : 0) |
                        ((used_result_nonzero_components & 0b1100) ? 0b0010 : 0),
                    grad_operand_temp_pushed);
    if (used_result_nonzero_components & 0b0101) {
      a_.OpDerivRTXCoarse(
          dxbc::Dest::R(system_temp_result_, used_result_nonzero_components & 0b0101),
          grad_operand.SwizzleSwizzled(0b010000));
    }
    if (used_result_nonzero_components & 0b1010) {
      a_.OpDerivRTYCoarse(
          dxbc::Dest::R(system_temp_result_, used_result_nonzero_components & 0b1010),
          grad_operand.SwizzleSwizzled(0b01000000));
    }
    if (grad_operand_temp_pushed) {
      PopSystemTemp();
    }
    StoreResult(instr.result, dxbc::Src::R(system_temp_result_));
    return;
  }

  if (instr.opcode == FetchOpcode::kGetTextureBorderColorFrac) {
    REX_ASSERT_ALWAYS();
    EmitTranslationError("getBCF is unimplemented", false);
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  if (instr.opcode != FetchOpcode::kTextureFetch &&
      instr.opcode != FetchOpcode::kGetTextureBorderColorFrac &&
      instr.opcode != FetchOpcode::kGetTextureComputedLOD &&
      instr.opcode != FetchOpcode::kGetTextureWeights) {
    REX_UNHANDLED_CASE(instr.opcode);
    EmitTranslationError("Unknown texture fetch operation");
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  u32 tfetch_index = instr.operands[1].storage_index;

  bool use_computed_lod = instr.attributes.use_computed_lod &&
                          (IsPixelShader() || instr.attributes.use_register_gradients);
  if (instr.opcode == FetchOpcode::kGetTextureComputedLOD &&
      (!use_computed_lod || instr.attributes.use_register_gradients)) {
    REX_ASSERT_ALWAYS();
    EmitTranslationError("getCompTexLOD used with explicit LOD or gradients - contradicts MSDN",
                         false);
    StoreResult(instr.result, dxbc::Src::LF(0.0f));
    return;
  }

  // Offsets apply at LOD 0 as their 0.5 granularity prevents passing them as AOffImmI to `sample`.
  f32 offsets[3] = {};
  // getCompTexLOD takes no offsets.
  if (instr.opcode != FetchOpcode::kGetTextureComputedLOD) {
    const f32 rounding_offset = 1.5f / 1024.0f;
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        offsets[0] = instr.attributes.offset_x + rounding_offset;
        if (instr.opcode == FetchOpcode::kGetTextureWeights) {
          offsets[0] -= 0.5f;
        }
        break;
      case xenos::FetchOpDimension::k2D:
        offsets[0] = instr.attributes.offset_x + rounding_offset;
        offsets[1] = instr.attributes.offset_y + rounding_offset;
        if (instr.opcode == FetchOpcode::kGetTextureWeights) {
          offsets[0] -= 0.5f;
          offsets[1] -= 0.5f;
        }
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        offsets[0] = instr.attributes.offset_x + rounding_offset;
        offsets[1] = instr.attributes.offset_y + rounding_offset;
        offsets[2] = instr.attributes.offset_z + rounding_offset;
        if (instr.opcode == FetchOpcode::kGetTextureWeights) {
          offsets[0] -= 0.5f;
          offsets[1] -= 0.5f;
          offsets[2] -= 0.5f;
        }
        break;
      case xenos::FetchOpDimension::kCube:
        offsets[0] = instr.attributes.offset_x + rounding_offset;
        offsets[1] = instr.attributes.offset_y + rounding_offset;
        if (instr.opcode == FetchOpcode::kGetTextureWeights) {
          offsets[0] -= 0.5f;
          offsets[1] -= 0.5f;
        } else {
          offsets[2] = instr.attributes.offset_z;
        }
        break;
    }
  }
  u32 offsets_not_zero = 0b000;
  for (u32 i = 0; i < 3; ++i) {
    if (offsets[i] != 0.0f) {
      offsets_not_zero |= 1 << i;
    }
  }
  dxbc::Src offsets_src(dxbc::Src::LF(offsets[0], offsets[1], offsets[2], 0.0f));

  u32 size_needed_components = 0b0000;
  if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    if (!instr.attributes.unnormalized_coordinates) {
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          // Size is always needed for 1D textures to support wide 1D textures.
          size_needed_components |= 0b0001;
          break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          size_needed_components |= used_result_nonzero_components & 0b0011;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          size_needed_components |= used_result_nonzero_components & 0b0111;
          break;
      }
    }
  } else {
    size_needed_components |= offsets_not_zero;
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        if (instr.attributes.unnormalized_coordinates) {
          size_needed_components |= 0b0001;
        }
        break;
      case xenos::FetchOpDimension::k2D:
        if (instr.attributes.unnormalized_coordinates) {
          size_needed_components |= 0b0011;
        }
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        // Stacked and 3D textures use different SRVs, so the check is always needed.
        size_needed_components |= 0b1000;
        if (instr.attributes.unnormalized_coordinates) {
          size_needed_components |= 0b0111;
        } else {
          size_needed_components |= 0b0100;
        }
        break;
      case xenos::FetchOpDimension::kCube:
        if (instr.attributes.unnormalized_coordinates) {
          size_needed_components |= 0b0011;
        }
        size_needed_components &= 0b0011;
        break;
    }
  }
  if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked && size_needed_components) {
    // Stacked and 3D textures pack the size differently, so whether it is 3D is always needed.
    size_needed_components |= 0b1000;
  }
  u32 size_and_is_3d_temp = size_needed_components ? PushSystemTemp() : UINT32_MAX;
  if (size_needed_components) {
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        a_.OpUBFE(dxbc::Dest::R(size_and_is_3d_temp, 0b0001), dxbc::Src::LU(24), dxbc::Src::LU(0),
                  RequestTextureFetchConstantWord(tfetch_index, 2));
        break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube:
        a_.OpUBFE(dxbc::Dest::R(size_and_is_3d_temp, size_needed_components),
                  dxbc::Src::LU(13, 13, 0, 0), dxbc::Src::LU(0, 13, 0, 0),
                  RequestTextureFetchConstantWord(tfetch_index, 2));
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        a_.OpUBFE(dxbc::Dest::R(size_and_is_3d_temp, 0b1000), dxbc::Src::LU(2), dxbc::Src::LU(9),
                  RequestTextureFetchConstantWord(tfetch_index, 5));
        a_.OpIEq(dxbc::Dest::R(size_and_is_3d_temp, 0b1000),
                 dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW),
                 dxbc::Src::LU(std::to_underlying(xenos::DataDimension::k3D)));
        if (size_needed_components & 0b0111) {
          a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
          a_.OpUBFE(dxbc::Dest::R(size_and_is_3d_temp, size_needed_components & 0b0111),
                    dxbc::Src::LU(11, 11, 10, 0), dxbc::Src::LU(0, 11, 22, 0),
                    RequestTextureFetchConstantWord(tfetch_index, 2));
          a_.OpElse();
          a_.OpUBFE(dxbc::Dest::R(size_and_is_3d_temp, size_needed_components & 0b0111),
                    dxbc::Src::LU(13, 13, 6, 0), dxbc::Src::LU(0, 13, 26, 0),
                    RequestTextureFetchConstantWord(tfetch_index, 2));
          a_.OpEndIf();
        }
        break;
    }
    if (size_needed_components & 0b0111) {
      // Fetch constants store the size minus 1.
      a_.OpIAdd(dxbc::Dest::R(size_and_is_3d_temp, size_needed_components & 0b0111),
                dxbc::Src::R(size_and_is_3d_temp), dxbc::Src::LU(1));
      a_.OpUToF(dxbc::Dest::R(size_and_is_3d_temp, size_needed_components & 0b0111),
                dxbc::Src::R(size_and_is_3d_temp));
    }
  }
  u32 revert_resolution_scale_axes =
      REXCVAR_GET(::rex::gpu::cvars::draw_resolution_scaled_texture_offsets)
          ? static_cast<u32>(draw_resolution_scale_x_ > 1) |
                (static_cast<u32>(draw_resolution_scale_y_ > 1) << 1)
          : 0;

  if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    REX_ASSERT((used_result_nonzero_components & 0b1000) == 0);

    bool coord_operand_temp_pushed = false;
    dxbc::Src coord_operand =
        LoadOperand(instr.operands[0], used_result_nonzero_components, coord_operand_temp_pushed);
    dxbc::Src coord_src(coord_operand);
    u32 resolution_scaled_result_components =
        used_result_nonzero_components & revert_resolution_scale_axes;
    u32 resolution_scaled_coord_components =
        instr.attributes.unnormalized_coordinates ? resolution_scaled_result_components : 0b0000;
    u32 resolution_scaled_size_components =
        size_needed_components & resolution_scaled_result_components;
    if (resolution_scaled_coord_components || resolution_scaled_size_components) {
      if (resolution_scaled_coord_components && (coord_src.type != dxbc::OperandType::kTemp ||
                                                 coord_src.index_1d.index != system_temp_result_)) {
        a_.OpMov(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components), coord_src);
        coord_src = dxbc::Src::R(system_temp_result_);
      }
      REX_ASSERT((used_result_nonzero_components & 0b1000) == 0);
      a_.OpAnd(dxbc::Dest::R(system_temp_result_, 0b1000),
               LoadSystemConstant(SystemConstants::Index::kTexturesResolutionScaled,
                                  offsetof(SystemConstants, textures_resolution_scaled),
                                  dxbc::Src::kXXXX),
               dxbc::Src::LU(static_cast<u32>(1) << tfetch_index));
      a_.OpIf(true, dxbc::Src::R(system_temp_result_, dxbc::Src::kWWWW));
      dxbc::Src resolution_scale_src(dxbc::Src::LF(static_cast<f32>(draw_resolution_scale_x_),
                                                   static_cast<f32>(draw_resolution_scale_y_), 1.0f,
                                                   1.0f));
      if (resolution_scaled_coord_components) {
        a_.OpMul(dxbc::Dest::R(system_temp_result_, resolution_scaled_coord_components), coord_src,
                 resolution_scale_src);
      }
      if (resolution_scaled_size_components) {
        a_.OpMul(dxbc::Dest::R(size_and_is_3d_temp, resolution_scaled_size_components),
                 dxbc::Src::R(size_and_is_3d_temp), resolution_scale_src);
      }
      a_.OpEndIf();
    }
    u32 offsets_needed = offsets_not_zero & used_result_nonzero_components;
    if (!instr.attributes.unnormalized_coordinates || offsets_needed) {
      coord_src = dxbc::Src::R(system_temp_result_);
      dxbc::Dest coord_dest(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components));
      if (instr.attributes.unnormalized_coordinates) {
        if (offsets_needed) {
          a_.OpAdd(coord_dest, coord_operand, offsets_src);
        }
      } else {
        REX_ASSERT((size_needed_components & used_result_nonzero_components) ==
                   used_result_nonzero_components);
        if (offsets_needed) {
          a_.OpMAd(coord_dest, coord_operand, dxbc::Src::R(size_and_is_3d_temp), offsets_src);
        } else {
          a_.OpMul(coord_dest, coord_operand, dxbc::Src::R(size_and_is_3d_temp));
        }
      }
    }
    a_.OpFrc(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components), coord_src);
    if (coord_operand_temp_pushed) {
      PopSystemTemp();
    }
  } else {
    dxbc::Src signs_uint_src(GetSystemConstantSrc(
        offsetof(SystemConstants, texture_swizzled_signs) + sizeof(u32) * (tfetch_index >> 2),
        dxbc::Src::kXXXX));
    u32 signs_shift = (tfetch_index & 3) * 8;
    u32 signs_temp = UINT32_MAX;
    if (instr.opcode == FetchOpcode::kTextureFetch) {
      signs_temp = PushSystemTemp();
      MarkSystemConstantUsed(SystemConstants::Index::kTextureSwizzledSigns);
      a_.OpUBFE(dxbc::Dest::R(signs_temp, used_result_nonzero_components), dxbc::Src::LU(2),
                dxbc::Src::LU(signs_shift, signs_shift + 2, signs_shift + 4, signs_shift + 6),
                signs_uint_src);
    }

    // The bindless sampler index is loaded to W after the coordinates, so W is a coordinate temp.
    u32 coord_and_sampler_temp = PushSystemTemp();

    bool coord_operand_temp_pushed = false;
    dxbc::Src coord_operand = LoadOperand(
        instr.operands[0], (1 << xenos::GetFetchOpDimensionComponentCount(instr.dimension)) - 1,
        coord_operand_temp_pushed);
    u32 normalized_components = 0b0000;
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        normalized_components = 0b0001;
        break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube:
        normalized_components = 0b0011;
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        normalized_components = 0b0111;
        break;
    }
    u32 normalized_components_with_offsets = normalized_components & offsets_not_zero;
    u32 normalized_components_with_scaled_offsets =
        normalized_components_with_offsets & revert_resolution_scale_axes;
    u32 normalized_components_with_unscaled_offsets =
        normalized_components_with_offsets & ~normalized_components_with_scaled_offsets;
    u32 normalized_components_without_offsets =
        normalized_components & ~normalized_components_with_offsets;
    if (instr.attributes.unnormalized_coordinates) {
      REX_ASSERT(normalized_components != 0);
      REX_ASSERT((size_needed_components & normalized_components) == normalized_components);
      if (normalized_components_with_offsets) {
        if (normalized_components_with_scaled_offsets) {
          a_.OpAnd(dxbc::Dest::R(coord_and_sampler_temp, 0b1000),
                   LoadSystemConstant(SystemConstants::Index::kTexturesResolutionScaled,
                                      offsetof(SystemConstants, textures_resolution_scaled),
                                      dxbc::Src::kXXXX),
                   dxbc::Src::LU(static_cast<u32>(1) << tfetch_index));
          a_.OpIf(true, dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW));
          a_.OpAdd(
              dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_scaled_offsets),
              coord_operand,
              dxbc::Src::LF(offsets[0] / static_cast<f32>(draw_resolution_scale_x_),
                            offsets[1] / static_cast<f32>(draw_resolution_scale_y_), 0.0f, 0.0f));
          a_.OpElse();
          a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_scaled_offsets),
                   coord_operand, offsets_src);
          a_.OpEndIf();
        }
        if (normalized_components_with_unscaled_offsets) {
          a_.OpAdd(
              dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_unscaled_offsets),
              coord_operand, offsets_src);
        }
        if (normalized_components_without_offsets) {
          a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, normalized_components_without_offsets),
                   coord_operand);
        }
        REX_ASSERT((normalized_components & 0b011) != 0);
        a_.OpDiv(dxbc::Dest::R(coord_and_sampler_temp, normalized_components & 0b011),
                 dxbc::Src::R(coord_and_sampler_temp), dxbc::Src::R(size_and_is_3d_temp));
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          REX_ASSERT((size_needed_components & 0b1100) == 0b1100);
          a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
          a_.OpDiv(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                   dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ),
                   dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ));
          a_.OpEndIf();
        }
      } else {
        a_.OpDiv(dxbc::Dest::R(coord_and_sampler_temp, normalized_components), coord_operand,
                 dxbc::Src::R(size_and_is_3d_temp));
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ),
                    coord_operand.SelectFromSwizzled(2));
        }
      }
    } else {
      if (normalized_components_with_offsets) {
        REX_ASSERT((size_needed_components & normalized_components_with_offsets) ==
                   normalized_components_with_offsets);
        a_.OpDiv(dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_offsets),
                 offsets_src, dxbc::Src::R(size_and_is_3d_temp));
        if (normalized_components_with_scaled_offsets) {
          a_.OpAnd(dxbc::Dest::R(coord_and_sampler_temp, 0b1000),
                   LoadSystemConstant(SystemConstants::Index::kTexturesResolutionScaled,
                                      offsetof(SystemConstants, textures_resolution_scaled),
                                      dxbc::Src::kXXXX),
                   dxbc::Src::LU(static_cast<u32>(1) << tfetch_index));
          a_.OpIf(true, dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW));
          a_.OpMAd(dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_scaled_offsets),
                   dxbc::Src::R(coord_and_sampler_temp),
                   dxbc::Src::LF(1.0f / static_cast<f32>(draw_resolution_scale_x_),
                                 1.0f / static_cast<f32>(draw_resolution_scale_y_), 1.0f, 1.0f),
                   coord_operand);
          a_.OpElse();
          a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_scaled_offsets),
                   coord_operand, dxbc::Src::R(coord_and_sampler_temp));
          a_.OpEndIf();
        }
        if (normalized_components_with_unscaled_offsets) {
          a_.OpAdd(
              dxbc::Dest::R(coord_and_sampler_temp, normalized_components_with_unscaled_offsets),
              coord_operand, dxbc::Src::R(coord_and_sampler_temp));
        }
      }
      if (normalized_components_without_offsets & 0b011) {
        a_.OpMov(
            dxbc::Dest::R(coord_and_sampler_temp, normalized_components_without_offsets & 0b011),
            coord_operand);
      }
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        REX_ASSERT((size_needed_components & 0b1100) == 0b1100);
        if (normalized_components_with_offsets & 0b100) {
          a_.OpIf(false, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
          a_.OpMAd(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                   coord_operand.SelectFromSwizzled(2),
                   dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ), dxbc::Src::LF(offsets[2]));
          a_.OpEndIf();
        } else {
          a_.OpMul(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                   coord_operand.SelectFromSwizzled(2),
                   dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ));
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW),
                    coord_operand.SelectFromSwizzled(2),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
        }
      }
    }
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
        a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b0110), dxbc::Src::LF(0.0f));
        break;
      case xenos::FetchOpDimension::k2D:
        a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b0100), dxbc::Src::LF(0.0f));
        break;
      case xenos::FetchOpDimension::kCube: {
        // SC/TC plus 1 moves from 1...2 to -1...1.
        a_.OpMAd(dxbc::Dest::R(coord_and_sampler_temp, 0b0011),
                 dxbc::Src::R(coord_and_sampler_temp), dxbc::Src::LF(2.0f), dxbc::Src::LF(-3.0f));
        if (offsets[2] != 0.0f) {
          a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                   coord_operand.SelectFromSwizzled(2), dxbc::Src::LF(offsets[2]));
          a_.OpFToU(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
        } else {
          a_.OpFToU(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    coord_operand.SelectFromSwizzled(2));
        }
        a_.OpUMin(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                  dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ), dxbc::Src::LU(5));
        a_.OpUBFE(dxbc::Dest::R(coord_and_sampler_temp, 0b1100), dxbc::Src::LU(0, 0, 2, 1),
                  dxbc::Src::LU(0, 0, 1, 0),
                  dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
        // Axes are remapped opposite to the ALU cube instruction.
        a_.OpSwitch(dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
        a_.OpCase(dxbc::Src::LU(0));
        {
          a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b0010),
                   -dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kYYYY));
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kXXXX),
                    -dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kXXXX));
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0001),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW), dxbc::Src::LF(-1.0f),
                    dxbc::Src::LF(1.0f));
        }
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(1));
        {
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW),
                    -dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kYYYY),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kYYYY));
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0010),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW), dxbc::Src::LF(-1.0f),
                    dxbc::Src::LF(1.0f));
        }
        a_.OpBreak();
        a_.OpDefault();
        {
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0001),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW),
                    -dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kXXXX),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kXXXX));
          a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b0010),
                   -dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kYYYY));
          a_.OpMovC(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                    dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kWWWW), dxbc::Src::LF(-1.0f),
                    dxbc::Src::LF(1.0f));
        }
        a_.OpBreak();
        a_.OpEndSwitch();
      } break;
      default:
        break;
    }
    if (coord_operand_temp_pushed) {
      PopSystemTemp();
    }

    if (instr.opcode == FetchOpcode::kGetTextureComputedLOD) {
      u32 sampler_binding_index = FindOrAddSamplerBinding(
          tfetch_index, instr.attributes.mag_filter, instr.attributes.min_filter,
          xenos::TextureFilter::kLinear, instr.attributes.aniso_filter);
      dxbc::Src sampler(dxbc::Src::S(sampler_binding_index, sampler_binding_index));
      if (bindless_resources_used_) {
        if (cbuffer_index_descriptor_indices_ == kBindingIndexUnallocated) {
          cbuffer_index_descriptor_indices_ = cbuffer_count_++;
        }
        u32 sampler_bindless_descriptor_index =
            sampler_bindings_[sampler_binding_index].bindless_descriptor_index;
        a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b1000),
                 dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                               std::to_underlying(CbufferRegister::kDescriptorIndices),
                               sampler_bindless_descriptor_index >> 2)
                     .Select(sampler_bindless_descriptor_index & 3));
        sampler = dxbc::Src::S(0, dxbc::Index(coord_and_sampler_temp, 3));
      }
      u32 is_unsigned_temp = PushSystemTemp();
      MarkSystemConstantUsed(SystemConstants::Index::kTextureSwizzledSigns);
      a_.OpUBFE(dxbc::Dest::R(is_unsigned_temp, 0b0001), dxbc::Src::LU(8),
                dxbc::Src::LU(signs_shift), signs_uint_src);
      a_.OpINE(dxbc::Dest::R(is_unsigned_temp, 0b0001),
               dxbc::Src::R(is_unsigned_temp, dxbc::Src::kXXXX),
               dxbc::Src::LU(std::to_underlying(xenos::TextureSign::kSigned) * 0b01010101));
      if (bindless_resources_used_) {
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
          a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
        }
        for (u32 is_stacked = 0;
             is_stacked < (instr.dimension == xenos::FetchOpDimension::k3DOrStacked ? 2u : 1u);
             ++is_stacked) {
          xenos::FetchOpDimension srv_dimension = instr.dimension;
          if (is_stacked) {
            srv_dimension = xenos::FetchOpDimension::k2D;
            a_.OpElse();
          }
          u32 texture_binding_index_unsigned =
              FindOrAddTextureBinding(tfetch_index, srv_dimension, false);
          u32 texture_binding_index_signed =
              FindOrAddTextureBinding(tfetch_index, srv_dimension, true);
          u32 texture_bindless_descriptor_index_unsigned =
              texture_bindings_[texture_binding_index_unsigned].bindless_descriptor_index;
          u32 texture_bindless_descriptor_index_signed =
              texture_bindings_[texture_binding_index_signed].bindless_descriptor_index;
          if (cbuffer_index_descriptor_indices_ == kBindingIndexUnallocated) {
            cbuffer_index_descriptor_indices_ = cbuffer_count_++;
          }
          a_.OpMovC(dxbc::Dest::R(is_unsigned_temp, 0b0001),
                    dxbc::Src::R(is_unsigned_temp, dxbc::Src::kXXXX),
                    dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                                  std::to_underlying(CbufferRegister::kDescriptorIndices),
                                  texture_bindless_descriptor_index_unsigned >> 2)
                        .Select(texture_bindless_descriptor_index_unsigned & 3),
                    dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                                  std::to_underlying(CbufferRegister::kDescriptorIndices),
                                  texture_bindless_descriptor_index_signed >> 2)
                        .Select(texture_bindless_descriptor_index_signed & 3));
          // Direct3D 12 ignores the array layer for LOD, and `lod` returns the unclamped LOD in Y.
          REX_ASSERT(used_result_nonzero_components == 0b0001);
          u32* bindless_srv_index = nullptr;
          switch (srv_dimension) {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D:
              bindless_srv_index = &srv_index_bindless_textures_2d_;
              break;
            case xenos::FetchOpDimension::k3DOrStacked:
              bindless_srv_index = &srv_index_bindless_textures_3d_;
              break;
            case xenos::FetchOpDimension::kCube:
              bindless_srv_index = &srv_index_bindless_textures_cube_;
              break;
          }
          REX_ASSERT(bindless_srv_index != nullptr);
          if (*bindless_srv_index == kBindingIndexUnallocated) {
            *bindless_srv_index = srv_count_++;
          }
          a_.OpLOD(
              dxbc::Dest::R(system_temp_result_, 0b0001), dxbc::Src::R(coord_and_sampler_temp), 3,
              dxbc::Src::T(*bindless_srv_index, dxbc::Index(is_unsigned_temp, 0), dxbc::Src::kYYYY),
              sampler);
        }
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          a_.OpEndIf();
        }
      } else {
        a_.OpIf(true, dxbc::Src::R(is_unsigned_temp, dxbc::Src::kXXXX));
        for (u32 is_signed = 0; is_signed < 2; ++is_signed) {
          if (is_signed) {
            a_.OpElse();
          }
          if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
            REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
            a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
          }
          for (u32 is_stacked = 0;
               is_stacked < (instr.dimension == xenos::FetchOpDimension::k3DOrStacked ? 2u : 1u);
               ++is_stacked) {
            if (is_stacked) {
              a_.OpElse();
            }
            REX_ASSERT(used_result_nonzero_components == 0b0001);
            u32 texture_binding_index = FindOrAddTextureBinding(
                tfetch_index, is_stacked ? xenos::FetchOpDimension::k2D : instr.dimension,
                is_signed != 0);
            a_.OpLOD(dxbc::Dest::R(system_temp_result_, 0b0001),
                     dxbc::Src::R(coord_and_sampler_temp), 3,
                     dxbc::Src::T(texture_bindings_[texture_binding_index].bindful_srv_index,
                                  std::to_underlying(SRVMainRegister::kBindfulTexturesStart) +
                                      texture_binding_index,
                                  dxbc::Src::kYYYY),
                     sampler);
          }
          if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
            a_.OpEndIf();
          }
        }
        a_.OpEndIf();
      }
      PopSystemTemp();
    } else {
      dxbc::Src lod_src(dxbc::Src::LF(0.0f));
      u32 grad_component_count = 0;
      u32 grad_h_lod_temp = UINT32_MAX;
      u32 grad_v_temp = UINT32_MAX;
      if (instr.attributes.mip_filter != xenos::TextureFilter::kBaseMap) {
        grad_h_lod_temp = PushSystemTemp();
        lod_src = dxbc::Src::R(grad_h_lod_temp, dxbc::Src::kWWWW);
        // Explicit LOD adds specified LOD, sampler bias and instruction bias in 11.3 order.
        dxbc::Dest lod_dest(dxbc::Dest::R(grad_h_lod_temp, 0b1000));
        // Fetch constant LOD bias is stored multiplied by 32.
        a_.OpIBFE(lod_dest, dxbc::Src::LU(10), dxbc::Src::LU(12),
                  RequestTextureFetchConstantWord(tfetch_index, 4));
        a_.OpIToF(lod_dest, lod_src);
        if (instr.attributes.use_register_lod) {
          a_.OpMAd(lod_dest, lod_src, dxbc::Src::LF(1.0f / 32.0f),
                   dxbc::Src::R(system_temp_grad_h_lod_, dxbc::Src::kWWWW));
          if (instr.attributes.lod_bias != 0.0f) {
            a_.OpAdd(lod_dest, lod_src, dxbc::Src::LF(instr.attributes.lod_bias));
          }
        } else {
          if (instr.attributes.lod_bias != 0.0f) {
            a_.OpMAd(lod_dest, lod_src, dxbc::Src::LF(1.0f / 32.0f),
                     dxbc::Src::LF(instr.attributes.lod_bias));
          } else {
            a_.OpMul(lod_dest, lod_src, dxbc::Src::LF(1.0f / 32.0f));
          }
        }
        if (use_computed_lod) {
          grad_v_temp = PushSystemTemp();
          switch (instr.dimension) {
            case xenos::FetchOpDimension::k1D:
              grad_component_count = 1;
              break;
            case xenos::FetchOpDimension::k2D:
              grad_component_count = 2;
              break;
            case xenos::FetchOpDimension::k3DOrStacked:
            case xenos::FetchOpDimension::kCube:
              grad_component_count = 3;
              break;
          }
          REX_ASSERT(grad_component_count != 0);
          u32 grad_mask = (1 << grad_component_count) - 1;
          a_.OpExp(lod_dest, lod_src);
          if (instr.attributes.use_register_gradients) {
            // Register gradients are in the cube space for cube maps.
            a_.OpMul(dxbc::Dest::R(grad_h_lod_temp, grad_mask),
                     dxbc::Src::R(system_temp_grad_h_lod_), lod_src);
            a_.OpMul(dxbc::Dest::R(grad_v_temp, grad_mask),
                     dxbc::Src::R(system_temp_grad_v_vfetch_address_), lod_src);
            if (instr.attributes.unnormalized_coordinates &&
                instr.dimension != xenos::FetchOpDimension::kCube) {
              u32 grad_norm_mask = grad_mask;
              if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
                grad_norm_mask &= 0b0011;
              }
              REX_ASSERT((size_needed_components & grad_norm_mask) == grad_norm_mask);
              a_.OpDiv(dxbc::Dest::R(grad_h_lod_temp, grad_norm_mask),
                       dxbc::Src::R(grad_h_lod_temp), dxbc::Src::R(size_and_is_3d_temp));
              a_.OpDiv(dxbc::Dest::R(grad_v_temp, grad_norm_mask), dxbc::Src::R(grad_v_temp),
                       dxbc::Src::R(size_and_is_3d_temp));
              REX_ASSERT((size_needed_components & 0b1100) == 0b1100);
              a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
              a_.OpDiv(dxbc::Dest::R(grad_h_lod_temp, 0b0100),
                       dxbc::Src::R(grad_h_lod_temp, dxbc::Src::kZZZZ),
                       dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ));
              a_.OpDiv(dxbc::Dest::R(grad_v_temp, 0b0100),
                       dxbc::Src::R(grad_v_temp, dxbc::Src::kZZZZ),
                       dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ));
              a_.OpEndIf();
            }
          } else {
            // Implicit gradients use coarse derivatives, as Direct3D 11.3 sampling does.
            a_.OpDerivRTXCoarse(dxbc::Dest::R(grad_h_lod_temp, grad_mask),
                                dxbc::Src::R(coord_and_sampler_temp));
            a_.OpMul(dxbc::Dest::R(grad_h_lod_temp, grad_mask), dxbc::Src::R(grad_h_lod_temp),
                     lod_src);
            a_.OpDerivRTYCoarse(dxbc::Dest::R(grad_v_temp, grad_mask),
                                dxbc::Src::R(coord_and_sampler_temp));
            a_.OpMul(dxbc::Dest::R(grad_v_temp, grad_mask), dxbc::Src::R(grad_v_temp), lod_src);
          }
          if (instr.dimension == xenos::FetchOpDimension::k1D) {
            a_.OpMov(dxbc::Dest::R(grad_h_lod_temp, 0b0010), dxbc::Src::LF(0.0f));
            a_.OpMov(dxbc::Dest::R(grad_v_temp, 0b0010), dxbc::Src::LF(0.0f));
            grad_component_count = 2;
          }
        }
      }

      // D3D12 disallows anisotropic with point filtering, so explicit LOD disables anisotropy.
      u32 sampler_binding_index = FindOrAddSamplerBinding(
          tfetch_index, instr.attributes.mag_filter, instr.attributes.min_filter,
          instr.attributes.mip_filter,
          use_computed_lod ? instr.attributes.aniso_filter : xenos::AnisoFilter::kDisabled);
      dxbc::Src sampler(dxbc::Src::S(sampler_binding_index, sampler_binding_index));
      if (bindless_resources_used_) {
        if (cbuffer_index_descriptor_indices_ == kBindingIndexUnallocated) {
          cbuffer_index_descriptor_indices_ = cbuffer_count_++;
        }
        u32 sampler_bindless_descriptor_index =
            sampler_bindings_[sampler_binding_index].bindless_descriptor_index;
        a_.OpMov(dxbc::Dest::R(coord_and_sampler_temp, 0b1000),
                 dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                               std::to_underlying(CbufferRegister::kDescriptorIndices),
                               sampler_bindless_descriptor_index >> 2)
                     .Select(sampler_bindless_descriptor_index & 3));
        sampler = dxbc::Src::S(0, dxbc::Index(coord_and_sampler_temp, 3));
      }

      a_.OpMov(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components),
               dxbc::Src::LF(0.0f));

      u32 is_signed_temp = PushSystemTemp();
      a_.OpIEq(dxbc::Dest::R(is_signed_temp, used_result_nonzero_components),
               dxbc::Src::R(signs_temp),
               dxbc::Src::LU(std::to_underlying(xenos::TextureSign::kSigned)));

      dxbc::Src layer_lerp_factor_src(dxbc::Src::LF(0.0f));
      u32 srv_selection_temp = bindless_resources_used_ ? PushSystemTemp() : UINT32_MAX;
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        bool vol_mag_filter_is_fetch_const =
            instr.attributes.vol_mag_filter == xenos::TextureFilter::kUseFetchConst;
        bool vol_min_filter_is_fetch_const =
            instr.attributes.vol_min_filter == xenos::TextureFilter::kUseFetchConst;
        bool vol_mag_filter_is_linear =
            instr.attributes.vol_mag_filter == xenos::TextureFilter::kLinear;
        bool vol_min_filter_is_linear =
            instr.attributes.vol_min_filter == xenos::TextureFilter::kLinear;
        if (grad_v_temp != UINT32_MAX &&
            (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const ||
             vol_mag_filter_is_linear != vol_min_filter_is_linear)) {
          if (srv_selection_temp == UINT32_MAX) {
            srv_selection_temp = PushSystemTemp();
          }
          layer_lerp_factor_src = dxbc::Src::R(srv_selection_temp, dxbc::Src::kZZZZ);
          a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b0100), dxbc::Src::LF(0.0f));
          REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
          a_.OpIf(false, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
          a_.OpMax(dxbc::Dest::R(srv_selection_temp, 0b1000),
                   dxbc::Src::R(grad_h_lod_temp, dxbc::Src::kZZZZ),
                   dxbc::Src::R(grad_v_temp, dxbc::Src::kZZZZ));
          if (!instr.attributes.unnormalized_coordinates) {
            REX_ASSERT((size_needed_components & 0b0100) == 0b0100);
            a_.OpMul(dxbc::Dest::R(srv_selection_temp, 0b1000),
                     dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW),
                     dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kZZZZ));
          }
          // NaN is treated as magnification, and zero srv_selection_temp.w means magnifying.
          a_.OpLT(dxbc::Dest::R(srv_selection_temp, 0b1000), dxbc::Src::LF(1.0f),
                  dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
          if (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const) {
            a_.OpIf(false, dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
            if (vol_mag_filter_is_fetch_const) {
              a_.OpAnd(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       RequestTextureFetchConstantWord(tfetch_index, 4), dxbc::Src::LU(1));
            } else {
              a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       dxbc::Src::LU(static_cast<u32>(vol_mag_filter_is_linear)));
            }
            a_.OpElse();
            if (vol_min_filter_is_fetch_const) {
              a_.OpUBFE(dxbc::Dest::R(srv_selection_temp, 0b1000), dxbc::Src::LU(1),
                        dxbc::Src::LU(1), RequestTextureFetchConstantWord(tfetch_index, 4));
            } else {
              a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       dxbc::Src::LU(static_cast<u32>(vol_min_filter_is_linear)));
            }
            a_.OpEndIf();
            a_.OpIf(true, dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
          } else if (vol_mag_filter_is_linear) {
            REX_ASSERT(!vol_min_filter_is_linear);
            a_.OpIf(false, dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
          } else {
            REX_ASSERT(vol_min_filter_is_linear);
            REX_ASSERT(!vol_mag_filter_is_linear);
            a_.OpIf(true, dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
          }
          a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                   dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ), dxbc::Src::LF(-0.5f));
          a_.OpFrc(dxbc::Dest::R(srv_selection_temp, 0b0100),
                   dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
          a_.OpEndIf();
          a_.OpEndIf();
        } else {
          // No gradients assumes magnification (LOD 0), LOD stays within 2D layers unlike 3D mips.
          if (vol_mag_filter_is_fetch_const || vol_mag_filter_is_linear) {
            if (srv_selection_temp == UINT32_MAX) {
              srv_selection_temp = PushSystemTemp();
            }
            layer_lerp_factor_src = dxbc::Src::R(srv_selection_temp, dxbc::Src::kZZZZ);
            a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b0100), dxbc::Src::LF(0.0f));
            REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
            a_.OpIf(false, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
            if (vol_mag_filter_is_fetch_const) {
              a_.OpAnd(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       RequestTextureFetchConstantWord(tfetch_index, 4), dxbc::Src::LU(1));
              a_.OpIf(true, dxbc::Src::R(srv_selection_temp, dxbc::Src::kWWWW));
            }
            a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                     dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ), dxbc::Src::LF(-0.5f));
            a_.OpFrc(dxbc::Dest::R(srv_selection_temp, 0b0100),
                     dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
            if (vol_mag_filter_is_fetch_const) {
              a_.OpEndIf();
            }
            a_.OpEndIf();
          }
        }
      }
      u32 result_first_component;
      rex::bit_scan_forward(used_result_nonzero_components, &result_first_component);
      dxbc::Src is_all_signed_src(dxbc::Src::R(is_signed_temp).Select(result_first_component));
      dxbc::Src is_any_signed_src(dxbc::Src::R(is_signed_temp).Select(result_first_component));
      if (used_result_nonzero_components != (1 << result_first_component)) {
        if (srv_selection_temp == UINT32_MAX) {
          srv_selection_temp = PushSystemTemp();
        }
        dxbc::Dest is_all_signed_dest(dxbc::Dest::R(srv_selection_temp, 0b0001));
        dxbc::Dest is_any_signed_dest(dxbc::Dest::R(srv_selection_temp, 0b0010));
        u32 result_remaining_components =
            used_result_nonzero_components & ~(static_cast<u32>(1) << result_first_component);
        u32 result_component;
        while (rex::bit_scan_forward(result_remaining_components, &result_component)) {
          result_remaining_components &= ~(static_cast<u32>(1) << result_component);
          a_.OpAnd(is_all_signed_dest, is_all_signed_src,
                   dxbc::Src::R(is_signed_temp).Select(result_component));
          a_.OpOr(is_any_signed_dest, is_any_signed_src,
                  dxbc::Src::R(is_signed_temp).Select(result_component));
          is_all_signed_src = dxbc::Src::R(srv_selection_temp, dxbc::Src::kXXXX);
          is_any_signed_src = dxbc::Src::R(srv_selection_temp, dxbc::Src::kYYYY);
        }
      }

      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        REX_ASSERT((size_needed_components & 0b1000) == 0b1000);
        a_.OpIf(true, dxbc::Src::R(size_and_is_3d_temp, dxbc::Src::kWWWW));
      }
      for (u32 is_stacked = 0;
           is_stacked < (instr.dimension == xenos::FetchOpDimension::k3DOrStacked ? 2u : 1u);
           ++is_stacked) {
        xenos::FetchOpDimension srv_dimension = instr.dimension;
        u32 srv_grad_component_count = grad_component_count;
        bool layer_lerp_needed = false;
        if (is_stacked) {
          srv_dimension = xenos::FetchOpDimension::k2D;
          srv_grad_component_count = 2;
          layer_lerp_needed = layer_lerp_factor_src.type != dxbc::OperandType::kImmediate32;
          a_.OpElse();
          // Floor the layer index, Xenos addresses layers like 3D unlike D3D12 nearest even.
          a_.OpRoundNI(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                       dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ));
        }
        u32 texture_binding_index_unsigned =
            FindOrAddTextureBinding(tfetch_index, srv_dimension, false);
        u32 texture_binding_index_signed =
            FindOrAddTextureBinding(tfetch_index, srv_dimension, true);
        const TextureBinding& texture_binding_unsigned =
            texture_bindings_[texture_binding_index_unsigned];
        const TextureBinding& texture_binding_signed =
            texture_bindings_[texture_binding_index_signed];
        dxbc::Src srv_unsigned(dxbc::Src::LF(0.0f)), srv_signed(dxbc::Src::LF(0.0f));
        if (bindless_resources_used_) {
          u32* bindless_srv_index = nullptr;
          switch (srv_dimension) {
            case xenos::FetchOpDimension::k1D:
            case xenos::FetchOpDimension::k2D:
              bindless_srv_index = &srv_index_bindless_textures_2d_;
              break;
            case xenos::FetchOpDimension::k3DOrStacked:
              bindless_srv_index = &srv_index_bindless_textures_3d_;
              break;
            case xenos::FetchOpDimension::kCube:
              bindless_srv_index = &srv_index_bindless_textures_cube_;
              break;
          }
          REX_ASSERT(bindless_srv_index != nullptr);
          if (*bindless_srv_index == kBindingIndexUnallocated) {
            *bindless_srv_index = srv_count_++;
          }
          REX_ASSERT(srv_selection_temp != UINT32_MAX);
          srv_unsigned = dxbc::Src::T(*bindless_srv_index, dxbc::Index(srv_selection_temp, 3));
          srv_signed = srv_unsigned;
        } else {
          srv_unsigned = dxbc::Src::T(texture_binding_unsigned.bindful_srv_index,
                                      std::to_underlying(SRVMainRegister::kBindfulTexturesStart) +
                                          texture_binding_index_unsigned);
          srv_signed = dxbc::Src::T(texture_binding_signed.bindful_srv_index,
                                    std::to_underlying(SRVMainRegister::kBindfulTexturesStart) +
                                        texture_binding_index_signed);
        }
        for (u32 layer = 0; layer < (layer_lerp_needed ? 2u : 1u); ++layer) {
          u32 layer_value_temp = system_temp_result_;
          if (layer) {
            layer_value_temp = PushSystemTemp();
            a_.OpNE(dxbc::Dest::R(layer_value_temp, 0b0001), layer_lerp_factor_src,
                    dxbc::Src::LF(0.0f));
            a_.OpIf(true, dxbc::Src::R(layer_value_temp, dxbc::Src::kXXXX));
            a_.OpAdd(dxbc::Dest::R(coord_and_sampler_temp, 0b0100),
                     dxbc::Src::R(coord_and_sampler_temp, dxbc::Src::kZZZZ), dxbc::Src::LF(1.0f));
          }
          a_.OpIf(false, is_all_signed_src);
          {
            if (bindless_resources_used_) {
              REX_ASSERT(srv_selection_temp != UINT32_MAX);
              if (cbuffer_index_descriptor_indices_ == kBindingIndexUnallocated) {
                cbuffer_index_descriptor_indices_ = cbuffer_count_++;
              }
              u32 texture_bindless_descriptor_index =
                  texture_binding_unsigned.bindless_descriptor_index;
              a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                                     std::to_underlying(CbufferRegister::kDescriptorIndices),
                                     texture_bindless_descriptor_index >> 2)
                           .Select(texture_bindless_descriptor_index & 3));
            }
            if (grad_v_temp != UINT32_MAX) {
              REX_ASSERT(grad_component_count != 0);
              a_.OpSampleD(dxbc::Dest::R(layer_value_temp, used_result_nonzero_components),
                           dxbc::Src::R(coord_and_sampler_temp), 3, srv_unsigned, sampler,
                           dxbc::Src::R(grad_h_lod_temp), dxbc::Src::R(grad_v_temp),
                           srv_grad_component_count);
            } else {
              a_.OpSampleL(dxbc::Dest::R(layer_value_temp, used_result_nonzero_components),
                           dxbc::Src::R(coord_and_sampler_temp), 3, srv_unsigned, sampler, lod_src);
            }
          }
          a_.OpEndIf();
          a_.OpIf(true, is_any_signed_src);
          {
            u32 signed_temp = PushSystemTemp();
            if (bindless_resources_used_) {
              REX_ASSERT(srv_selection_temp != UINT32_MAX);
              if (cbuffer_index_descriptor_indices_ == kBindingIndexUnallocated) {
                cbuffer_index_descriptor_indices_ = cbuffer_count_++;
              }
              u32 texture_bindless_descriptor_index =
                  texture_binding_signed.bindless_descriptor_index;
              a_.OpMov(dxbc::Dest::R(srv_selection_temp, 0b1000),
                       dxbc::Src::CB(cbuffer_index_descriptor_indices_,
                                     std::to_underlying(CbufferRegister::kDescriptorIndices),
                                     texture_bindless_descriptor_index >> 2)
                           .Select(texture_bindless_descriptor_index & 3));
            }
            if (grad_v_temp != UINT32_MAX) {
              REX_ASSERT(grad_component_count != 0);
              a_.OpSampleD(dxbc::Dest::R(signed_temp, used_result_nonzero_components),
                           dxbc::Src::R(coord_and_sampler_temp), 3, srv_signed, sampler,
                           dxbc::Src::R(grad_h_lod_temp), dxbc::Src::R(grad_v_temp),
                           srv_grad_component_count);
            } else {
              a_.OpSampleL(dxbc::Dest::R(signed_temp, used_result_nonzero_components),
                           dxbc::Src::R(coord_and_sampler_temp), 3, srv_signed, sampler, lod_src);
            }
            a_.OpMovC(dxbc::Dest::R(layer_value_temp, used_result_nonzero_components),
                      dxbc::Src::R(is_signed_temp), dxbc::Src::R(signed_temp),
                      dxbc::Src::R(layer_value_temp));
            PopSystemTemp();
          }
          a_.OpEndIf();
          if (layer) {
            REX_ASSERT(layer_value_temp != system_temp_result_);
            a_.OpAdd(dxbc::Dest::R(layer_value_temp, used_result_nonzero_components),
                     dxbc::Src::R(layer_value_temp), -dxbc::Src::R(system_temp_result_));
            a_.OpMAd(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components),
                     dxbc::Src::R(layer_value_temp), layer_lerp_factor_src,
                     dxbc::Src::R(system_temp_result_));
            a_.OpEndIf();
            PopSystemTemp();
          }
        }
      }
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        a_.OpEndIf();
      }

      if (srv_selection_temp != UINT32_MAX) {
        PopSystemTemp();
      }
      PopSystemTemp();

      if (grad_v_temp != UINT32_MAX) {
        PopSystemTemp();
      }
      if (grad_h_lod_temp != UINT32_MAX) {
        PopSystemTemp();
      }
    }

    PopSystemTemp();

    // Gamma is applied after filtering as host sampler filtering is outside translator control.
    if (instr.opcode == FetchOpcode::kTextureFetch) {
      REX_ASSERT(signs_temp != UINT32_MAX);
      for (u32 i = 0; i < 4; ++i) {
        if (!(used_result_nonzero_components & (1 << i))) {
          continue;
        }
        dxbc::Dest component_dest(dxbc::Dest::R(system_temp_result_, 1 << i));
        dxbc::Src component_src(dxbc::Src::R(system_temp_result_).Select(i));
        a_.OpSwitch(dxbc::Src::R(signs_temp).Select(i));
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::TextureSign::kUnsignedBiased)));
        a_.OpMAd(component_dest, component_src, dxbc::Src::LF(2.0f), dxbc::Src::LF(-1.0f));
        a_.OpBreak();
        a_.OpCase(dxbc::Src::LU(std::to_underlying(xenos::TextureSign::kGamma)));
        u32 gamma_temp = PushSystemTemp();
        PWLGammaToLinear(a_, system_temp_result_, i, system_temp_result_, i, false, gamma_temp, 0,
                         gamma_temp, 1);
        PopSystemTemp();
        a_.OpBreak();
        a_.OpEndSwitch();
      }
    }
    if (signs_temp != UINT32_MAX) {
      PopSystemTemp();
    }
  }

  if (size_and_is_3d_temp != UINT32_MAX) {
    PopSystemTemp();
  }

  if (instr.opcode == FetchOpcode::kTextureFetch) {
    u32 exp_adjust_temp = PushSystemTemp();
    a_.OpIBFE(dxbc::Dest::R(exp_adjust_temp, 0b0001), dxbc::Src::LU(6), dxbc::Src::LU(13),
              RequestTextureFetchConstantWord(tfetch_index, 3));
    a_.OpIMAd(dxbc::Dest::R(exp_adjust_temp, 0b0001),
              dxbc::Src::R(exp_adjust_temp, dxbc::Src::kXXXX),
              dxbc::Src::LI(static_cast<i32>(1) << 23), dxbc::Src::LF(1.0f));
    a_.OpMul(dxbc::Dest::R(system_temp_result_, used_result_nonzero_components),
             dxbc::Src::R(system_temp_result_), dxbc::Src::R(exp_adjust_temp, dxbc::Src::kXXXX));
    PopSystemTemp();
  }

  u32 used_result_zero_components = used_result_components & ~used_result_nonzero_components;
  if (used_result_zero_components) {
    a_.OpMov(dxbc::Dest::R(system_temp_result_, used_result_zero_components), dxbc::Src::LF(0.0f));
  }
  StoreResult(instr.result, dxbc::Src::R(system_temp_result_));
}

}  // namespace rex::gpu
