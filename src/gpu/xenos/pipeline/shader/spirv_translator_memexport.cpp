/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <SPIRV/GLSL.std.450.h>

#include <rex/assert.h>
#include <rex/xenos/ucode.h>
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/bit.h>

namespace rex::gpu {

void SPIRVShaderTranslator::ExportToMemory(u8 export_em) {
  if (!export_em) {
    return;
  }

  REX_ASSERT((export_em & ~GetCurrentShader().GetMemExportEMWritten()) == 0);

  if (!IsMemExportSupported()) {
    return;
  }

  std::optional<SPIRVBuilder::IfBuilder> if_memexport_allowed;
  if (main_memexport_allowed_ != spv::NoResult) {
    if_memexport_allowed.emplace(main_memexport_allowed_, spv::SelectionControlDontFlattenMask,
                                 *builder_);
  }

  std::optional<SPIRVBuilder::IfBuilder> if_pixel_not_killed;
  if (var_main_kill_pixel_ != spv::NoResult) {
    if_pixel_not_killed.emplace(
        builder_->createUnaryOp(spv::OpLogicalNot, type_bool,
                                builder_->createLoad(var_main_kill_pixel_, spv::NoPrecision)),
        spv::SelectionControlDontFlattenMask, *builder_);
  }

  spv::Id ea_vector =
      builder_->createUnaryOp(spv::OpBitcast, type_uint4,
                              builder_->createLoad(var_main_memexport_address_, spv::NoPrecision));
  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeUintConstant(30));
  id_vector_temp_.push_back(builder_->makeUintConstant(23));
  id_vector_temp_.push_back(id_vector_temp_.back());
  id_vector_temp_.push_back(id_vector_temp_.back());
  spv::Id address_validation_shift = builder_->makeCompositeConstant(type_uint4, id_vector_temp_);
  id_vector_temp_.clear();
  id_vector_temp_.push_back(builder_->makeUintConstant(0x1));
  id_vector_temp_.push_back(builder_->makeUintConstant(0x96));
  id_vector_temp_.push_back(id_vector_temp_.back());
  id_vector_temp_.push_back(id_vector_temp_.back());
  spv::Id address_validation_value = builder_->makeCompositeConstant(type_uint4, id_vector_temp_);
  SPIRVBuilder::IfBuilder if_address_valid(
      builder_->createUnaryOp(
          spv::OpAll, type_bool,
          builder_->createBinOp(spv::OpIEqual, type_bool4,
                                builder_->createBinOp(spv::OpShiftRightLogical, type_uint4,
                                                      ea_vector, address_validation_shift),
                                address_validation_value)),
      spv::SelectionControlDontFlattenMask, *builder_, 2, 1);

  using EMIdArray = std::array<spv::Id, xenos::ucode::kMaxMemExportElementCount>;

  auto for_each_em = [&]<std::invocable<u32> F>(F&& fn) {
    u8 em_remaining = export_em;
    u32 em_index;
    while (rex::bit_scan_forward(em_remaining, &em_index)) {
      em_remaining &= ~(static_cast<u8>(1) << em_index);
      fn(em_index);
    }
  };

  EMIdArray em_original;
  for_each_em([&](u32 em_index) {
    em_original[em_index] =
        builder_->createLoad(var_main_memexport_data_[em_index], spv::NoPrecision);
  });

  spv::Id format_word = builder_->createCompositeExtract(ea_vector, type_uint, 2);
  spv::Id swap_red_blue = builder_->createBinOp(
      spv::OpINotEqual, type_bool,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint, format_word,
                            builder_->makeUintConstant(static_cast<u32>(1) << 19)),
      const_uint_0_);
  EMIdArray em_swapped;
  uint_vector_temp_.clear();
  uint_vector_temp_.push_back(2);
  uint_vector_temp_.push_back(1);
  uint_vector_temp_.push_back(0);
  uint_vector_temp_.push_back(3);
  for_each_em([&](u32 em_index) {
    em_swapped[em_index] = builder_->createTriOp(
        spv::OpSelect, type_float4, swap_red_blue,
        builder_->createRvalueSwizzle(spv::NoPrecision, type_float4, em_original[em_index],
                                      uint_vector_temp_),
        em_original[em_index]);
  });

  spv::Id is_signed = builder_->createBinOp(
      spv::OpINotEqual, type_bool,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint, format_word,
                            builder_->makeUintConstant(static_cast<u32>(1) << 16)),
      const_uint_0_);
  spv::Id is_norm = builder_->createBinOp(
      spv::OpIEqual, type_bool,
      builder_->createBinOp(spv::OpBitwiseAnd, type_uint, format_word,
                            builder_->makeUintConstant(static_cast<u32>(1) << 17)),
      const_uint_0_);

  auto flush_nan = [&](const EMIdArray& em) -> EMIdArray {
    EMIdArray em_flushed;
    for_each_em([&](u32 em_index) {
      spv::Id element_unflushed = em[em_index];
      unsigned int component_count = builder_->getNumComponents(element_unflushed);
      em_flushed[em_index] = builder_->createTriOp(
          spv::OpSelect, type_float_vectors[component_count - 1],
          builder_->createUnaryOp(spv::OpIsNan, type_bool_vectors[component_count - 1],
                                  element_unflushed),
          const_float_vectors_0[component_count - 1], element_unflushed);
    });
    return em_flushed;
  };

  auto make_float_constant_vectors = [&](f32 value) -> std::array<spv::Id, 4> {
    std::array<spv::Id, 4> const_vectors;
    const_vectors[0] = builder_->makeFloatConstant(value);
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_vectors[0]);
    for (unsigned int component_count_minus_1 = 1; component_count_minus_1 < 4;
         ++component_count_minus_1) {
      id_vector_temp_.push_back(const_vectors[0]);
      const_vectors[component_count_minus_1] = builder_->makeCompositeConstant(
          type_float_vectors[component_count_minus_1], id_vector_temp_);
    }
    return const_vectors;
  };
  std::array<spv::Id, 4> const_float_vectors_minus_1 = make_float_constant_vectors(-1.0f);
  std::array<spv::Id, 4> const_float_vectors_minus_0_5 = make_float_constant_vectors(-0.5f);
  std::array<spv::Id, 4> const_float_vectors_0_5 = make_float_constant_vectors(0.5f);

  auto pack_8_16_32 = [&](std::array<u32, 4> widths) -> EMIdArray {
    unsigned int component_count;
    std::array<u32, 4> offsets{};
    for (component_count = 0; component_count < widths.size(); ++component_count) {
      if (!widths[component_count]) {
        break;
      }
      assert(widths[component_count] <= 23);
      if (component_count) {
        offsets[component_count] = offsets[component_count - 1] + widths[component_count - 1];
      }
    }
    REX_ASSERT(component_count != 0);

    EMIdArray em_unflushed = em_swapped;
    if (component_count < 4) {
      if (component_count == 1) {
        for_each_em([&](u32 em_index) {
          em_unflushed[em_index] =
              builder_->createCompositeExtract(em_unflushed[em_index], type_float, 0);
        });
      } else {
        uint_vector_temp_.clear();
        for (unsigned int component_index = 0; component_index < component_count;
             ++component_index) {
          uint_vector_temp_.push_back(component_index);
        }
        for_each_em([&](u32 em_index) {
          em_unflushed[em_index] = builder_->createRvalueSwizzle(
              spv::NoPrecision, type_float_vectors[component_count - 1], em_unflushed[em_index],
              uint_vector_temp_);
        });
      }
    }

    EMIdArray em_flushed = flush_nan(em_unflushed);

    SPIRVBuilder::IfBuilder if_signed(is_signed, spv::SelectionControlDontFlattenMask, *builder_);
    EMIdArray em_signed;
    {
      SPIRVBuilder::IfBuilder if_norm(is_norm, spv::SelectionControlDontFlattenMask, *builder_);
      EMIdArray em_norm;
      {
        id_vector_temp_.clear();
        for (unsigned int component_index = 0; component_index < component_count;
             ++component_index) {
          id_vector_temp_.push_back(builder_->makeFloatConstant(
              static_cast<f32>((static_cast<u32>(1) << (widths[component_index] - 1)) - 1)));
        }
        spv::Id const_max_value =
            component_count > 1 ? builder_->makeCompositeConstant(
                                      type_float_vectors[component_count - 1], id_vector_temp_)
                                : id_vector_temp_.front();
        for_each_em([&](u32 em_index) {
          em_norm[em_index] = builder_->CreateNoContractionBinOp(
              spv::OpFMul, type_float_vectors[component_count - 1],
              builder_->CreateTriBuiltinCall(
                  type_float_vectors[component_count - 1], ext_inst_glsl_std_450_, GLSLstd450FClamp,
                  em_flushed[em_index], const_float_vectors_minus_1[component_count - 1],
                  const_float_vectors_1[component_count - 1]),
              const_max_value);
        });
      }
      if_norm.MakeEndIf();
      for_each_em([&](u32 em_index) {
        em_signed[em_index] = if_norm.CreateMergePhi(em_norm[em_index], em_flushed[em_index]);
      });
      for_each_em([&](u32 em_index) {
        em_signed[em_index] = builder_->createUnaryOp(
            spv::OpBitcast, type_uint_vectors[component_count - 1],
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int_vectors[component_count - 1],
                builder_->CreateNoContractionBinOp(
                    spv::OpFAdd, type_float_vectors[component_count - 1], em_signed[em_index],
                    builder_->createTriOp(
                        spv::OpSelect, type_float_vectors[component_count - 1],
                        builder_->createBinOp(
                            spv::OpFOrdLessThan, type_bool_vectors[component_count - 1],
                            em_signed[em_index], const_float_vectors_0[component_count - 1]),
                        const_float_vectors_minus_0_5[component_count - 1],
                        const_float_vectors_0_5[component_count - 1]))));
      });
    }
    if_signed.MakeBeginElse();
    EMIdArray em_unsigned;
    {
      SPIRVBuilder::IfBuilder if_norm(is_norm, spv::SelectionControlDontFlattenMask, *builder_);
      EMIdArray em_norm;
      {
        id_vector_temp_.clear();
        for (unsigned int component_index = 0; component_index < component_count;
             ++component_index) {
          id_vector_temp_.push_back(builder_->makeFloatConstant(
              static_cast<f32>((static_cast<u32>(1) << widths[component_index]) - 1)));
        }
        spv::Id const_max_value =
            component_count > 1 ? builder_->makeCompositeConstant(
                                      type_float_vectors[component_count - 1], id_vector_temp_)
                                : id_vector_temp_.front();
        for_each_em([&](u32 em_index) {
          em_norm[em_index] = builder_->CreateNoContractionBinOp(
              spv::OpFMul, type_float_vectors[component_count - 1],
              builder_->CreateTriBuiltinCall(
                  type_float_vectors[component_count - 1], ext_inst_glsl_std_450_, GLSLstd450FClamp,
                  em_flushed[em_index], const_float_vectors_0[component_count - 1],
                  const_float_vectors_1[component_count - 1]),
              const_max_value);
        });
      }
      if_norm.MakeEndIf();
      for_each_em([&](u32 em_index) {
        em_unsigned[em_index] = if_norm.CreateMergePhi(em_norm[em_index], em_flushed[em_index]);
      });
      for_each_em([&](u32 em_index) {
        em_unsigned[em_index] = builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint_vectors[component_count - 1],
            builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float_vectors[component_count - 1],
                                               em_unsigned[em_index],
                                               const_float_vectors_0_5[component_count - 1]));
      });
    }
    if_signed.MakeEndIf();
    EMIdArray em_unpacked;
    for_each_em([&](u32 em_index) {
      em_unpacked[em_index] = if_signed.CreateMergePhi(em_signed[em_index], em_unsigned[em_index]);
    });

    EMIdArray em_packed;
    for_each_em([&](u32 em_index) {
      spv::Id element_unpacked = em_unpacked[em_index];
      em_packed[em_index] = component_count > 1
                                ? builder_->createCompositeExtract(element_unpacked, type_uint, 0)
                                : element_unpacked;
      for (unsigned int component_index = 1; component_index < component_count; ++component_index) {
        em_packed[em_index] = builder_->CreateQuadOp(
            spv::OpBitFieldInsert, type_uint, em_packed[em_index],
            builder_->createCompositeExtract(element_unpacked, type_uint, component_index),
            builder_->makeUintConstant(offsets[component_index]),
            builder_->makeUintConstant(widths[component_index]));
      }
      id_vector_temp_.clear();
      id_vector_temp_.resize(4, const_uint_0_);
      id_vector_temp_.front() = em_packed[em_index];
      em_packed[em_index] = builder_->createCompositeConstruct(type_uint4, id_vector_temp_);
    });

    return em_packed;
  };

  SPIRVBuilder::SwitchBuilder format_switch(
      builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, format_word,
                            builder_->makeUintConstant(8), builder_->makeUintConstant(6)),
      spv::SelectionControlDontFlattenMask, *builder_);

  struct FormatCase {
    EMIdArray em_packed;
    u32 element_bytes_log2;
    spv::Id phi_parent;
  };
  std::vector<FormatCase> format_cases;
  auto add_format_case = [&](const EMIdArray& em_packed, u32 element_bytes_log2) {
    FormatCase& format_case = format_cases.emplace_back();
    format_case.em_packed = em_packed;
    format_case.element_bytes_log2 = element_bytes_log2;
    format_case.phi_parent = builder_->getBuildPoint()->getId();
  };

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_8));
  format_switch.AddCurrentCaseLiteral(static_cast<unsigned int>(xenos::ColorFormat::k_8_A));
  format_switch.AddCurrentCaseLiteral(static_cast<unsigned int>(xenos::ColorFormat::k_8_B));
  add_format_case(pack_8_16_32({8}), 0);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_1_5_5_5));
  add_format_case(pack_8_16_32({5, 5, 5, 1}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_5_6_5));
  add_format_case(pack_8_16_32({5, 6, 5}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_6_5_5));
  add_format_case(pack_8_16_32({5, 5, 6}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_8_8_8_8));
  format_switch.AddCurrentCaseLiteral(static_cast<unsigned int>(xenos::ColorFormat::k_8_8_8_8_A));
  format_switch.AddCurrentCaseLiteral(
      static_cast<unsigned int>(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16));
  add_format_case(pack_8_16_32({8, 8, 8, 8}), 2);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_2_10_10_10));
  format_switch.AddCurrentCaseLiteral(
      static_cast<unsigned int>(xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16));
  add_format_case(pack_8_16_32({10, 10, 10, 2}), 2);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_8_8));
  add_format_case(pack_8_16_32({8, 8}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_4_4_4_4));
  add_format_case(pack_8_16_32({4, 4, 4, 4}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_10_11_11));
  format_switch.AddCurrentCaseLiteral(
      static_cast<unsigned int>(xenos::ColorFormat::k_10_11_11_AS_16_16_16_16));
  add_format_case(pack_8_16_32({11, 11, 10}), 2);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_11_11_10));
  format_switch.AddCurrentCaseLiteral(
      static_cast<unsigned int>(xenos::ColorFormat::k_11_11_10_AS_16_16_16_16));
  add_format_case(pack_8_16_32({10, 11, 11}), 2);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16));
  add_format_case(pack_8_16_32({16}), 1);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16_16));
  add_format_case(pack_8_16_32({16, 16}), 2);

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16_16_16_16));
  {
    EMIdArray fixed16_flushed = flush_nan(em_swapped);

    SPIRVBuilder::IfBuilder if_signed(is_signed, spv::SelectionControlDontFlattenMask, *builder_);
    EMIdArray fixed16_signed;
    {
      SPIRVBuilder::IfBuilder if_norm(is_norm, spv::SelectionControlDontFlattenMask, *builder_);
      EMIdArray fixed16_norm;
      {
        id_vector_temp_.clear();
        id_vector_temp_.resize(4, builder_->makeFloatConstant(
                                      static_cast<f32>((static_cast<u32>(1) << (16 - 1)) - 1)));
        spv::Id const_snorm16_max_value =
            builder_->makeCompositeConstant(type_float4, id_vector_temp_);
        for_each_em([&](u32 em_index) {
          fixed16_norm[em_index] = builder_->CreateNoContractionBinOp(
              spv::OpFMul, type_float4,
              builder_->CreateTriBuiltinCall(type_float4, ext_inst_glsl_std_450_, GLSLstd450FClamp,
                                             fixed16_flushed[em_index],
                                             const_float_vectors_minus_1[3], const_float4_1),
              const_snorm16_max_value);
        });
      }
      if_norm.MakeEndIf();
      for_each_em([&](u32 em_index) {
        fixed16_signed[em_index] =
            if_norm.CreateMergePhi(fixed16_norm[em_index], fixed16_flushed[em_index]);
      });
      for_each_em([&](u32 em_index) {
        fixed16_signed[em_index] = builder_->createUnaryOp(
            spv::OpBitcast, type_uint4,
            builder_->createUnaryOp(
                spv::OpConvertFToS, type_int4,
                builder_->CreateNoContractionBinOp(
                    spv::OpFAdd, type_float4, fixed16_signed[em_index],
                    builder_->createTriOp(
                        spv::OpSelect, type_float4,
                        builder_->createBinOp(spv::OpFOrdLessThan, type_bool4,
                                              fixed16_signed[em_index], const_float4_0),
                        const_float_vectors_minus_0_5[3], const_float_vectors_0_5[3]))));
      });
    }
    if_signed.MakeBeginElse();
    EMIdArray fixed16_unsigned;
    {
      SPIRVBuilder::IfBuilder if_norm(is_norm, spv::SelectionControlDontFlattenMask, *builder_);
      EMIdArray fixed16_norm;
      {
        id_vector_temp_.clear();
        id_vector_temp_.resize(
            4, builder_->makeFloatConstant(static_cast<f32>((static_cast<u32>(1) << 16) - 1)));
        spv::Id const_unorm16_max_value =
            builder_->makeCompositeConstant(type_float4, id_vector_temp_);
        for_each_em([&](u32 em_index) {
          fixed16_norm[em_index] = builder_->CreateNoContractionBinOp(
              spv::OpFMul, type_float4,
              builder_->CreateTriBuiltinCall(type_float4, ext_inst_glsl_std_450_, GLSLstd450FClamp,
                                             fixed16_flushed[em_index], const_float4_0,
                                             const_float4_1),
              const_unorm16_max_value);
        });
      }
      if_norm.MakeEndIf();
      for_each_em([&](u32 em_index) {
        fixed16_unsigned[em_index] =
            if_norm.CreateMergePhi(fixed16_norm[em_index], fixed16_flushed[em_index]);
      });
      for_each_em([&](u32 em_index) {
        fixed16_unsigned[em_index] = builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint4,
            builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float4, fixed16_unsigned[em_index],
                                               const_float_vectors_0_5[3]));
      });
    }
    if_signed.MakeEndIf();
    EMIdArray fixed16_unpacked;
    for_each_em([&](u32 em_index) {
      fixed16_unpacked[em_index] =
          if_signed.CreateMergePhi(fixed16_signed[em_index], fixed16_unsigned[em_index]);
    });

    EMIdArray fixed16_packed;
    spv::Id const_uint_16 = builder_->makeUintConstant(16);
    for_each_em([&](u32 em_index) {
      spv::Id fixed16_element_unpacked = fixed16_unpacked[em_index];
      id_vector_temp_.clear();
      for (u32 component_index = 0; component_index < 2; ++component_index) {
        id_vector_temp_.push_back(builder_->CreateQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder_->createCompositeExtract(fixed16_element_unpacked, type_uint,
                                             2 * component_index),
            builder_->createCompositeExtract(fixed16_element_unpacked, type_uint,
                                             2 * component_index + 1),
            const_uint_16, const_uint_16));
      }
      for (u32 component_index = 2; component_index < 4; ++component_index) {
        id_vector_temp_.push_back(const_uint_0_);
      }
      fixed16_packed[em_index] = builder_->createCompositeConstruct(type_uint4, id_vector_temp_);
    });

    add_format_case(fixed16_packed, 3);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16_FLOAT));
  {
    EMIdArray format_packed_16_float;
    for_each_em([&](u32 em_index) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(
          builder_->createCompositeExtract(em_swapped[em_index], type_float, 0));
      id_vector_temp_.push_back(const_float_0);
      spv::Id format_packed_16_float_x = builder_->CreateUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450_, GLSLstd450PackHalf2x16,
          builder_->createCompositeConstruct(type_float2, id_vector_temp_));
      id_vector_temp_.clear();
      id_vector_temp_.resize(4, const_uint_0_);
      id_vector_temp_.front() = format_packed_16_float_x;
      format_packed_16_float[em_index] =
          builder_->createCompositeConstruct(type_uint4, id_vector_temp_);
    });
    add_format_case(format_packed_16_float, 1);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16_16_FLOAT));
  {
    EMIdArray format_packed_16_16_float;
    for_each_em([&](u32 em_index) {
      uint_vector_temp_.clear();
      uint_vector_temp_.push_back(0);
      uint_vector_temp_.push_back(1);
      spv::Id format_packed_16_16_float_xy = builder_->CreateUnaryBuiltinCall(
          type_uint, ext_inst_glsl_std_450_, GLSLstd450PackHalf2x16,
          builder_->createRvalueSwizzle(spv::NoPrecision, type_float2, em_swapped[em_index],
                                        uint_vector_temp_));
      id_vector_temp_.clear();
      id_vector_temp_.resize(4, const_uint_0_);
      id_vector_temp_.front() = format_packed_16_16_float_xy;
      format_packed_16_16_float[em_index] =
          builder_->createCompositeConstruct(type_uint4, id_vector_temp_);
    });
    add_format_case(format_packed_16_16_float, 2);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_16_16_16_16_FLOAT));
  {
    EMIdArray format_packed_16_16_16_16_float;
    for_each_em([&](u32 em_index) {
      spv::Id format_packed_16_16_16_16_float_xy_zw[2];
      for (u32 component_index = 0; component_index < 2; ++component_index) {
        uint_vector_temp_.clear();
        uint_vector_temp_.push_back(2 * component_index);
        uint_vector_temp_.push_back(2 * component_index + 1);
        format_packed_16_16_16_16_float_xy_zw[component_index] = builder_->CreateUnaryBuiltinCall(
            type_uint, ext_inst_glsl_std_450_, GLSLstd450PackHalf2x16,
            builder_->createRvalueSwizzle(spv::NoPrecision, type_float2, em_swapped[em_index],
                                          uint_vector_temp_));
      }
      id_vector_temp_.clear();
      id_vector_temp_.push_back(format_packed_16_16_16_16_float_xy_zw[0]);
      id_vector_temp_.push_back(format_packed_16_16_16_16_float_xy_zw[1]);
      id_vector_temp_.push_back(const_uint_0_);
      id_vector_temp_.push_back(const_uint_0_);
      format_packed_16_16_16_16_float[em_index] =
          builder_->createCompositeConstruct(type_uint4, id_vector_temp_);
    });
    add_format_case(format_packed_16_16_16_16_float, 3);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_32_FLOAT));
  {
    EMIdArray format_packed_32_float;
    for_each_em([&](u32 em_index) {
      format_packed_32_float[em_index] =
          builder_->createUnaryOp(spv::OpBitcast, type_uint4, em_swapped[em_index]);
    });
    add_format_case(format_packed_32_float, 2);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_32_32_FLOAT));
  {
    EMIdArray format_packed_32_32_float;
    for_each_em([&](u32 em_index) {
      format_packed_32_32_float[em_index] =
          builder_->createUnaryOp(spv::OpBitcast, type_uint4, em_swapped[em_index]);
    });
    add_format_case(format_packed_32_32_float, 3);
  }

  format_switch.MakeBeginCase(static_cast<unsigned int>(xenos::ColorFormat::k_32_32_32_32_FLOAT));
  {
    EMIdArray format_packed_32_32_32_32_float;
    for_each_em([&](u32 em_index) {
      format_packed_32_32_32_32_float[em_index] =
          builder_->createUnaryOp(spv::OpBitcast, type_uint4, em_swapped[em_index]);
    });
    add_format_case(format_packed_32_32_32_32_float, 4);
  }

  format_switch.MakeEndSwitch();

  EMIdArray em_packed;
  for_each_em([&](u32 em_index) {
    auto em_packed_phi =
        std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_uint4, spv::OpPhi);
    em_packed_phi->addIdOperand(const_uint4_0_);
    em_packed_phi->addIdOperand(format_switch.GetDefaultPhiParent());
    for (const FormatCase& format_case : format_cases) {
      em_packed_phi->addIdOperand(format_case.em_packed[em_index]);
      em_packed_phi->addIdOperand(format_case.phi_parent);
    }
    em_packed[em_index] = em_packed_phi->getResultId();
    builder_->getBuildPoint()->addInstruction(std::move(em_packed_phi));
  });
  spv::Id element_bytes_log2;
  {
    auto element_bytes_log2_phi =
        std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_uint, spv::OpPhi);
    element_bytes_log2_phi->addIdOperand(builder_->makeUintConstant(5));
    element_bytes_log2_phi->addIdOperand(format_switch.GetDefaultPhiParent());
    for (const FormatCase& format_case : format_cases) {
      element_bytes_log2_phi->addIdOperand(
          builder_->makeUintConstant(format_case.element_bytes_log2));
      element_bytes_log2_phi->addIdOperand(format_case.phi_parent);
    }
    element_bytes_log2 = element_bytes_log2_phi->getResultId();
    builder_->getBuildPoint()->addInstruction(std::move(element_bytes_log2_phi));
  }

  spv::Id endian = builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, format_word,
                                         const_uint_0_, builder_->makeUintConstant(3));
  for_each_em(
      [&](u32 em_index) { em_packed[em_index] = EndianSwap128Uint4(em_packed[em_index], endian); });

  spv::Id em0_index = builder_->createTriOp(
      spv::OpBitFieldUExtract, type_uint, builder_->createCompositeExtract(ea_vector, type_uint, 1),
      const_uint_0_, builder_->makeUintConstant(23));

  spv::Id em_indices_to_store = builder_->createTriOp(
      spv::OpBitFieldUExtract, type_uint,
      builder_->createLoad(var_main_memexport_data_written_, spv::NoPrecision), const_uint_0_,
      builder_->createUnaryOp(
          spv::OpBitcast, type_uint,
          builder_->CreateTriBuiltinCall(
              type_int, ext_inst_glsl_std_450_, GLSLstd450SClamp,
              builder_->createBinOp(
                  spv::OpISub, type_int,
                  builder_->createUnaryOp(
                      spv::OpBitcast, type_int,
                      builder_->createTriOp(
                          spv::OpBitFieldUExtract, type_uint,
                          builder_->createCompositeExtract(ea_vector, type_uint, 3), const_uint_0_,
                          builder_->makeUintConstant(23))),
                  builder_->createUnaryOp(spv::OpBitcast, type_int, em0_index)),
              const_int_0_, builder_->makeIntConstant(xenos::ucode::kMaxMemExportElementCount))));

  spv::Id const_uint_2 = builder_->makeUintConstant(2);
  spv::Id em0_address_bytes = builder_->createBinOp(
      spv::OpIAdd, type_uint,
      builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                            builder_->createCompositeExtract(ea_vector, type_uint, 0),
                            const_uint_2),
      builder_->createBinOp(spv::OpShiftLeftLogical, type_uint, em0_index, element_bytes_log2));

  auto store_needed_em = [&]<std::invocable<u32> F>(F&& fn) {
    for_each_em([&](u32 em_index) {
      SPIRVBuilder::IfBuilder if_em_needed(
          builder_->createBinOp(
              spv::OpINotEqual, type_bool,
              builder_->createBinOp(spv::OpBitwiseAnd, type_uint, em_indices_to_store,
                                    builder_->makeUintConstant(1u << em_index)),
              const_uint_0_),
          spv::SelectionControlDontFlattenMask, *builder_, 2, 1);
      fn(em_index);
      if_em_needed.MakeEndIf();
    });
  };
  SPIRVBuilder::SwitchBuilder element_size_switch(element_bytes_log2,
                                                  spv::SelectionControlDontFlattenMask, *builder_);
  element_size_switch.MakeBeginCase(0);
  {
    store_needed_em([&](u32 em_index) {
      spv::Id element_address_bytes =
          em_index != 0 ? builder_->createBinOp(spv::OpIAdd, type_uint, em0_address_bytes,
                                                builder_->makeUintConstant(em_index))
                        : em0_address_bytes;
      spv::Id replace_shift = builder_->CreateQuadOp(spv::OpBitFieldInsert, type_uint,
                                                     const_uint_0_, element_address_bytes,
                                                     builder_->makeUintConstant(3), const_uint_2);
      StoreUint32ToSharedMemory(
          builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                builder_->createCompositeExtract(em_packed[em_index], type_uint, 0),
                                replace_shift),
          builder_->createUnaryOp(spv::OpBitcast, type_int,
                                  builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                                                        element_address_bytes, const_uint_2)),
          builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                builder_->makeUintConstant(0xFFu), replace_shift));
    });
  }
  element_size_switch.MakeBeginCase(1);
  {
    spv::Id const_uint_1 = builder_->makeUintConstant(1);
    spv::Id em0_address_words =
        builder_->createBinOp(spv::OpShiftRightLogical, type_uint, em0_address_bytes, const_uint_1);
    store_needed_em([&](u32 em_index) {
      spv::Id element_address_words =
          em_index != 0 ? builder_->createBinOp(spv::OpIAdd, type_uint, em0_address_words,
                                                builder_->makeUintConstant(em_index))
                        : em0_address_words;
      spv::Id replace_shift = builder_->CreateQuadOp(spv::OpBitFieldInsert, type_uint,
                                                     const_uint_0_, element_address_words,
                                                     builder_->makeUintConstant(4), const_uint_1);
      StoreUint32ToSharedMemory(
          builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                builder_->createCompositeExtract(em_packed[em_index], type_uint, 0),
                                replace_shift),
          builder_->createUnaryOp(spv::OpBitcast, type_int,
                                  builder_->createBinOp(spv::OpShiftRightLogical, type_uint,
                                                        element_address_words, const_uint_1)),
          builder_->createBinOp(spv::OpShiftLeftLogical, type_uint,
                                builder_->makeUintConstant(0xFFFFu), replace_shift));
    });
  }
  element_size_switch.MakeBeginCase(2);
  {
    spv::Id em0_address_dwords =
        builder_->createBinOp(spv::OpShiftRightLogical, type_uint, em0_address_bytes, const_uint_2);
    store_needed_em([&](u32 em_index) {
      StoreUint32ToSharedMemory(
          builder_->createCompositeExtract(em_packed[em_index], type_uint, 0),
          builder_->createUnaryOp(
              spv::OpBitcast, type_int,
              em_index != 0 ? builder_->createBinOp(spv::OpIAdd, type_uint, em0_address_dwords,
                                                    builder_->makeUintConstant(em_index))
                            : em0_address_dwords));
    });
  }
  element_size_switch.MakeBeginCase(3);
  {
    spv::Id em0_address_dwords =
        builder_->createBinOp(spv::OpShiftRightLogical, type_uint, em0_address_bytes, const_uint_2);
    store_needed_em([&](u32 em_index) {
      spv::Id element_value = em_packed[em_index];
      spv::Id element_address_dwords_int = builder_->createUnaryOp(
          spv::OpBitcast, type_int,
          em_index != 0 ? builder_->createBinOp(spv::OpIAdd, type_uint, em0_address_dwords,
                                                builder_->makeUintConstant(2 * em_index))
                        : em0_address_dwords);
      StoreUint32ToSharedMemory(builder_->createCompositeExtract(element_value, type_uint, 0),
                                element_address_dwords_int);
      StoreUint32ToSharedMemory(
          builder_->createCompositeExtract(element_value, type_uint, 1),
          builder_->createBinOp(spv::OpIAdd, type_int, element_address_dwords_int,
                                builder_->makeIntConstant(1)));
    });
  }
  element_size_switch.MakeBeginCase(4);
  {
    spv::Id em0_address_dwords =
        builder_->createBinOp(spv::OpShiftRightLogical, type_uint, em0_address_bytes, const_uint_2);
    store_needed_em([&](u32 em_index) {
      spv::Id element_value = em_packed[em_index];
      spv::Id element_address_dwords_int = builder_->createUnaryOp(
          spv::OpBitcast, type_int,
          em_index != 0 ? builder_->createBinOp(spv::OpIAdd, type_uint, em0_address_dwords,
                                                builder_->makeUintConstant(4 * em_index))
                        : em0_address_dwords);
      StoreUint32ToSharedMemory(builder_->createCompositeExtract(element_value, type_uint, 0),
                                element_address_dwords_int);
      for (u32 element_dword_index = 1; element_dword_index < 4; ++element_dword_index) {
        StoreUint32ToSharedMemory(
            builder_->createCompositeExtract(element_value, type_uint, element_dword_index),
            builder_->createBinOp(
                spv::OpIAdd, type_int, element_address_dwords_int,
                builder_->makeIntConstant(static_cast<int>(element_dword_index))));
      }
    });
  }
  element_size_switch.MakeEndSwitch();

  if_address_valid.MakeEndIf();
  if (if_pixel_not_killed.has_value()) {
    if_pixel_not_killed->MakeEndIf();
  }
  if (if_memexport_allowed.has_value()) {
    if_memexport_allowed->MakeEndIf();
  }
}

}  // namespace rex::gpu
