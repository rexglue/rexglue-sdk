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

#include <climits>
#include <cmath>
#include <memory>
#include <sstream>
#include <utility>

#include <SPIRV/GLSL.std.450.h>
#include <format>

#include <rex/assert.h>
#include "gpu/xenos/pipeline/shader/spirv_translator.h"
#include <rex/math.h>

namespace rex::gpu {

void SPIRVShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  u32 used_result_components = instr.result.GetUsedResultComponents();
  u32 needed_words =
      xenos::GetVertexFormatNeededWords(instr.attributes.data_format, used_result_components);
  if (!needed_words && instr.is_mini_fetch) {
    StoreResult(instr.result, spv::NoResult);
    return;
  }

  StartNewBlockIfTerminated();

  u32 fetch_constant_word_0_index = instr.operands[1].storage_index << 1;

  spv::Id address;
  if (instr.is_mini_fetch) {
    address = builder_->createLoad(var_main_vfetch_address_, spv::NoPrecision);
  } else {
    id_vector_temp_.clear();
    id_vector_temp_.push_back(const_int_0_);
    id_vector_temp_.push_back(builder_->makeIntConstant(int(fetch_constant_word_0_index >> 2)));
    id_vector_temp_.push_back(builder_->makeIntConstant(int(fetch_constant_word_0_index & 3)));
    spv::Id fetch_constant_word_0 =
        builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                         uniform_fetch_constants_, id_vector_temp_),
                             spv::NoPrecision);
    address = builder_->createUnaryOp(
        spv::OpBitcast, type_int,
        builder_->createBinOp(spv::OpShiftRightLogical, type_uint, fetch_constant_word_0,
                              builder_->makeUintConstant(2)));
    if (instr.attributes.stride) {
      spv::Id index =
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0001);
      if (instr.attributes.is_index_rounded) {
        index = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, index,
                                                   builder_->makeFloatConstant(0.5f));
      }
      index =
          builder_->createUnaryOp(spv::OpConvertFToS, type_int,
                                  builder_->CreateUnaryBuiltinCall(
                                      type_float, ext_inst_glsl_std_450_, GLSLstd450Floor, index));
      if (instr.attributes.stride > 1) {
        index = builder_->createBinOp(spv::OpIMul, type_int, index,
                                      builder_->makeIntConstant(int(instr.attributes.stride)));
      }
      address = builder_->createBinOp(spv::OpIAdd, type_int, address, index);
    }
    builder_->createStore(address, var_main_vfetch_address_);
  }

  if (!needed_words) {
    StoreResult(instr.result, spv::NoResult);
    return;
  }

  unsigned int word_composite_indices[4] = {};
  spv::Id word_composite_constituents[4];
  u32 word_count = 0;
  u32 words_remaining = needed_words;
  u32 word_index;
  while (rex::bit_scan_forward(words_remaining, &word_index)) {
    words_remaining &= ~(1 << word_index);
    spv::Id word_address = address;
    i32 word_offset = instr.attributes.offset + static_cast<i32>(word_index);
    if (word_offset) {
      word_address = builder_->createBinOp(spv::OpIAdd, type_int, word_address,
                                           builder_->makeIntConstant(int(word_offset)));
    }
    word_composite_indices[word_index] = word_count;
    word_composite_constituents[word_count++] = LoadUint32FromSharedMemory(word_address);
  }
  spv::Id words;
  if (word_count > 1) {
    // Copy to id_vector_temp_ after the loop, as LoadUint32FromSharedMemory may use it.
    id_vector_temp_.clear();
    id_vector_temp_.insert(id_vector_temp_.cend(), word_composite_constituents,
                           word_composite_constituents + word_count);
    words = builder_->createCompositeConstruct(type_uint_vectors[word_count - 1], id_vector_temp_);
  } else {
    words = word_composite_constituents[0];
  }

  u32 fetch_constant_word_1_index = fetch_constant_word_0_index + 1;
  id_vector_temp_.clear();
  id_vector_temp_.push_back(const_int_0_);
  id_vector_temp_.push_back(builder_->makeIntConstant(int(fetch_constant_word_1_index >> 2)));
  id_vector_temp_.push_back(builder_->makeIntConstant(int(fetch_constant_word_1_index & 3)));
  spv::Id fetch_constant_word_1 =
      builder_->createLoad(builder_->createAccessChain(spv::StorageClassUniform,
                                                       uniform_fetch_constants_, id_vector_temp_),
                           spv::NoPrecision);
  words = EndianSwap32Uint(
      words, builder_->createBinOp(spv::OpBitwiseAnd, type_uint, fetch_constant_word_1,
                                   builder_->makeUintConstant(0b11)));

  spv::Id result = spv::NoResult;

  u32 used_format_components =
      used_result_components &
      ((1 << xenos::GetVertexFormatComponentCount(instr.attributes.data_format)) - 1);
  REX_ASSERT(used_format_components != 0);
  u32 used_format_component_count = rex::BitCount(used_format_components);
  spv::Id result_type = type_float_vectors[used_format_component_count - 1];
  bool format_is_packed = false;
  int packed_widths[4] = {}, packed_offsets[4] = {};
  u32 packed_words[4] = {};
  switch (instr.attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 8;
      packed_offsets[1] = 8;
      packed_offsets[2] = 16;
      packed_offsets[3] = 24;
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = 10;
      packed_widths[3] = 2;
      packed_offsets[1] = 10;
      packed_offsets[2] = 20;
      packed_offsets[3] = 30;
      break;
    case xenos::VertexFormat::k_10_11_11:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = 11;
      packed_widths[2] = 10;
      packed_offsets[1] = 11;
      packed_offsets[2] = 22;
      break;
    case xenos::VertexFormat::k_11_11_10:
      format_is_packed = true;
      packed_widths[0] = 10;
      packed_widths[1] = packed_widths[2] = 11;
      packed_offsets[1] = 10;
      packed_offsets[2] = 21;
      break;
    case xenos::VertexFormat::k_16_16:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = 16;
      packed_offsets[1] = 16;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 16;
      packed_offsets[1] = packed_offsets[3] = 16;
      packed_words[2] = packed_words[3] = 1;
      break;

    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
      spv::Id word_needed_component_values[2] = {};
      for (u32 i = 0; i < 2; ++i) {
        u32 word_needed_components = (used_format_components >> (i * 2)) & 0b11;
        if (!word_needed_components) {
          continue;
        }
        spv::Id word;
        if (word_count > 1) {
          word = builder_->createCompositeExtract(words, type_uint, word_composite_indices[i]);
        } else {
          word = words;
        }
        word = builder_->CreateUnaryBuiltinCall(type_float2, ext_inst_glsl_std_450_,
                                                GLSLstd450UnpackHalf2x16, word);
        if (word_needed_components != 0b11) {
          word = builder_->createCompositeExtract(word, type_float,
                                                  (word_needed_components & 0b01) ? 0 : 1);
        }
        word_needed_component_values[i] = word;
      }
      if (word_needed_component_values[1] == spv::NoResult) {
        result = word_needed_component_values[0];
      } else if (word_needed_component_values[0] == spv::NoResult) {
        result = word_needed_component_values[1];
      } else {
        // Vectors can be concatenated by createCompositeConstruct despite its assertion.
        std::unique_ptr<spv::Instruction> composite_construct_op =
            std::make_unique<spv::Instruction>(builder_->getUniqueId(), result_type,
                                               spv::OpCompositeConstruct);
        composite_construct_op->addIdOperand(word_needed_component_values[0]);
        composite_construct_op->addIdOperand(word_needed_component_values[1]);
        result = composite_construct_op->getResultId();
        builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
      }
    } break;

    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
      REX_ASSERT(used_format_components == needed_words);
      if (instr.attributes.is_signed) {
        result = builder_->createUnaryOp(spv::OpBitcast,
                                         type_int_vectors[used_format_component_count - 1], words);
        result = builder_->createUnaryOp(spv::OpConvertSToF, result_type, result);
      } else {
        result = builder_->createUnaryOp(spv::OpConvertUToF, result_type, words);
      }
      if (!instr.attributes.is_integer) {
        if (instr.attributes.is_signed) {
          switch (instr.attributes.signed_rf_mode) {
            case xenos::SignedRepeatingFractionMode::kZeroClampMinusOne:
              result = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, result_type, result,
                  builder_->makeFloatConstant(1.0f / 2147483647.0f));
              // 1/(2^31-1) rounds to 1/2^31 as float32, so no clamp to -1 when signed.
              break;
            case xenos::SignedRepeatingFractionMode::kNoZero: {
              result = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, result_type, result,
                  builder_->makeFloatConstant(1.0f / 2147483647.5f));
              spv::Id const_no_zero = builder_->makeFloatConstant(0.5f / 2147483647.5f);
              if (used_format_component_count > 1) {
                id_vector_temp_.clear();
                id_vector_temp_.insert(id_vector_temp_.cend(), used_format_component_count,
                                       const_no_zero);
                const_no_zero = builder_->makeCompositeConstant(result_type, id_vector_temp_);
              }
              result = builder_->CreateNoContractionBinOp(spv::OpFAdd, result_type, result,
                                                          const_no_zero);
            } break;
            default:
              REX_UNHANDLED_CASE(instr.attributes.signed_rf_mode);
          }
        } else {
          result =
              builder_->CreateNoContractionBinOp(spv::OpVectorTimesScalar, result_type, result,
                                                 builder_->makeFloatConstant(1.0f / 4294967295.0f));
        }
      }
      break;

    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      REX_ASSERT(used_format_components == needed_words);
      result = builder_->createUnaryOp(spv::OpBitcast, type_float_vectors[word_count - 1], words);
      break;

    default:
      REX_UNHANDLED_CASE(instr.attributes.data_format);
  }

  if (format_is_packed) {
    REX_ASSERT(result == spv::NoResult);
    if (instr.attributes.is_signed) {
      words = builder_->createUnaryOp(spv::OpBitcast, type_int_vectors[word_count - 1], words);
    }
    int extracted_widths[4] = {};
    spv::Id extracted_components[4] = {};
    u32 extracted_component_count = 0;
    unsigned int extraction_word_current_index = UINT_MAX;
    spv::Id extraction_word_current = words;
    for (u32 i = 0; i < 4; ++i) {
      if (!(used_format_components & (1 << i))) {
        continue;
      }
      if (word_count > 1) {
        unsigned int extraction_word_new_index = word_composite_indices[packed_words[i]];
        if (extraction_word_current_index != extraction_word_new_index) {
          extraction_word_current_index = extraction_word_new_index;
          extraction_word_current = builder_->createCompositeExtract(
              words, instr.attributes.is_signed ? type_int : type_uint, extraction_word_new_index);
        }
      }
      int extraction_width = packed_widths[i];
      REX_ASSERT(extraction_width != 0);
      extracted_widths[extracted_component_count] = extraction_width;
      extracted_components[extracted_component_count] = builder_->createTriOp(
          instr.attributes.is_signed ? spv::OpBitFieldSExtract : spv::OpBitFieldUExtract,
          instr.attributes.is_signed ? type_int : type_uint, extraction_word_current,
          builder_->makeIntConstant(packed_offsets[i]),
          builder_->makeIntConstant(extraction_width));
      ++extracted_component_count;
    }
    REX_ASSERT(extracted_component_count == used_format_component_count);
    if (used_format_component_count > 1) {
      id_vector_temp_.clear();
      id_vector_temp_.insert(id_vector_temp_.cend(), extracted_components,
                             extracted_components + used_format_component_count);
      result = builder_->createCompositeConstruct(
          instr.attributes.is_signed ? type_int_vectors[used_format_component_count - 1]
                                     : type_uint_vectors[used_format_component_count - 1],
          id_vector_temp_);
    } else {
      result = extracted_components[0];
    }
    result = builder_->createUnaryOp(
        instr.attributes.is_signed ? spv::OpConvertSToF : spv::OpConvertUToF, result_type, result);
    if (!instr.attributes.is_integer) {
      f32 packed_scales[4];
      bool packed_scales_same = true;
      for (u32 i = 0; i < used_format_component_count; ++i) {
        int extracted_width = extracted_widths[i];
        REX_ASSERT(extracted_width >= 2);
        packed_scales_same &= extracted_width != extracted_widths[0];
        f32 packed_scale_inv;
        if (instr.attributes.is_signed) {
          packed_scale_inv = static_cast<f32>((static_cast<u32>(1) << (extracted_width - 1)) - 1);
          if (instr.attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero) {
            packed_scale_inv += 0.5f;
          }
        } else {
          packed_scale_inv = static_cast<f32>((static_cast<u32>(1) << extracted_width) - 1);
        }
        packed_scales[i] = 1.0f / packed_scale_inv;
      }
      spv::Id const_packed_scale = builder_->makeFloatConstant(packed_scales[0]);
      spv::Op packed_scale_mul_op;
      if (used_format_component_count > 1) {
        if (packed_scales_same) {
          packed_scale_mul_op = spv::OpVectorTimesScalar;
        } else {
          packed_scale_mul_op = spv::OpFMul;
          id_vector_temp_.clear();
          id_vector_temp_.push_back(const_packed_scale);
          for (u32 i = 1; i < used_format_component_count; ++i) {
            id_vector_temp_.push_back(builder_->makeFloatConstant(packed_scales[i]));
          }
          const_packed_scale = builder_->makeCompositeConstant(result_type, id_vector_temp_);
        }
      } else {
        packed_scale_mul_op = spv::OpFMul;
      }
      result = builder_->CreateNoContractionBinOp(packed_scale_mul_op, result_type, result,
                                                  const_packed_scale);
      if (instr.attributes.is_signed) {
        switch (instr.attributes.signed_rf_mode) {
          case xenos::SignedRepeatingFractionMode::kZeroClampMinusOne: {
            spv::Id const_minus_1 = builder_->makeFloatConstant(-1.0f);
            if (used_format_component_count > 1) {
              id_vector_temp_.clear();
              id_vector_temp_.resize(used_format_component_count, const_minus_1);
              const_minus_1 = builder_->makeCompositeConstant(result_type, id_vector_temp_);
            }
            result = builder_->CreateBinBuiltinCall(result_type, ext_inst_glsl_std_450_,
                                                    GLSLstd450FMax, result, const_minus_1);
          } break;
          case xenos::SignedRepeatingFractionMode::kNoZero:
            id_vector_temp_.clear();
            for (u32 i = 0; i < used_format_component_count; ++i) {
              id_vector_temp_.push_back(builder_->makeFloatConstant(0.5f * packed_scales[i]));
            }
            result = builder_->CreateNoContractionBinOp(
                spv::OpFAdd, result_type, result,
                used_format_component_count > 1
                    ? builder_->makeCompositeConstant(result_type, id_vector_temp_)
                    : id_vector_temp_[0]);
            break;
          default:
            REX_UNHANDLED_CASE(instr.attributes.signed_rf_mode);
        }
      }
    }
  }

  if (result != spv::NoResult) {
    if (instr.attributes.exp_adjust) {
      result = builder_->CreateNoContractionBinOp(
          spv::OpVectorTimesScalar, builder_->getTypeId(result), result,
          builder_->makeFloatConstant(std::ldexp(1.0f, instr.attributes.exp_adjust)));
    }

    u32 used_missing_components = used_result_components & ~used_format_components;
    if (used_missing_components) {
      // Vectors can be concatenated by createCompositeConstruct despite its assertion.
      std::unique_ptr<spv::Instruction> composite_construct_op = std::make_unique<spv::Instruction>(
          builder_->getUniqueId(), type_float_vectors[rex::BitCount(used_result_components) - 1],
          spv::OpCompositeConstruct);
      composite_construct_op->addIdOperand(result);
      composite_construct_op->addIdOperand(
          const_float_vectors_0[rex::BitCount(used_missing_components) - 1]);
      result = composite_construct_op->getResultId();
      builder_->getBuildPoint()->addInstruction(std::move(composite_construct_op));
    }
  }
  StoreResult(instr.result, result);
}

void SPIRVShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  StartNewBlockIfTerminated();

  switch (instr.opcode) {
    case xenos::ucode::FetchOpcode::kSetTextureLOD:
      builder_->createStore(
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0001),
          var_main_tfetch_lod_);
      return;
    case xenos::ucode::FetchOpcode::kSetTextureGradientsHorz:
      builder_->createStore(
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0111),
          var_main_tfetch_gradients_h_);
      return;
    case xenos::ucode::FetchOpcode::kSetTextureGradientsVert:
      builder_->createStore(
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0111),
          var_main_tfetch_gradients_v_);
      return;
    default:
      break;
  }

  u32 used_result_components = instr.result.GetUsedResultComponents();
  u32 used_result_nonzero_components = instr.GetNonZeroResultComponents();
  switch (instr.opcode) {
    case xenos::ucode::FetchOpcode::kTextureFetch:
      break;
    case xenos::ucode::FetchOpcode::kGetTextureBorderColorFrac:
      REX_ASSERT_ALWAYS();
      EmitTranslationError("getBCF is unimplemented", false);
      used_result_nonzero_components = 0;
      break;
    case xenos::ucode::FetchOpcode::kGetTextureComputedLOD:
    case xenos::ucode::FetchOpcode::kGetTextureGradients:
      break;
    case xenos::ucode::FetchOpcode::kGetTextureWeights:
      used_result_nonzero_components &= ~static_cast<u32>(0b1000);
      break;
    default:
      REX_UNHANDLED_CASE(instr.opcode);
      EmitTranslationError("Unknown texture fetch operation");
      used_result_nonzero_components = 0;
  }
  u32 used_result_component_count = rex::BitCount(used_result_components);
  if (!used_result_nonzero_components) {
    if (used_result_components) {
      StoreResult(instr.result, const_float_vectors_0[used_result_component_count - 1]);
    }
    return;
  }

  spv::Id result[] = {const_float_0, const_float_0, const_float_0, const_float_0};

  if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureGradients) {
    spv::Id operand_0_storage = LoadOperandStorage(instr.operands[0]);
    bool derivative_function_x_used = (used_result_nonzero_components & 0b0011) != 0;
    bool derivative_function_y_used = (used_result_nonzero_components & 0b1100) != 0;
    spv::Id derivative_function_x = spv::NoResult;
    spv::Id derivative_function_y = spv::NoResult;
    if (derivative_function_x_used && derivative_function_y_used) {
      spv::Id derivative_function =
          GetOperandComponents(operand_0_storage, instr.operands[0], 0b0011);
      derivative_function_x = builder_->createCompositeExtract(derivative_function, type_float, 0);
      derivative_function_y = builder_->createCompositeExtract(derivative_function, type_float, 1);
    } else {
      if (derivative_function_x_used) {
        derivative_function_x = GetOperandComponents(operand_0_storage, instr.operands[0], 0b0001);
      }
      if (derivative_function_y_used) {
        derivative_function_y = GetOperandComponents(operand_0_storage, instr.operands[0], 0b0010);
      }
    }
    builder_->addCapability(spv::CapabilityDerivativeControl);
    u32 derivative_components_remaining = used_result_nonzero_components;
    u32 derivative_component_index;
    while (rex::bit_scan_forward(derivative_components_remaining, &derivative_component_index)) {
      derivative_components_remaining &= ~(UINT32_C(1) << derivative_component_index);
      result[derivative_component_index] = builder_->createUnaryOp(
          (derivative_component_index & 0b01) ? spv::OpDPdyCoarse : spv::OpDPdxCoarse, type_float,
          (derivative_component_index & 0b10) ? derivative_function_y : derivative_function_x);
    }
  } else {
    bool use_computed_lod = instr.attributes.use_computed_lod &&
                            (IsPixelShader() || instr.attributes.use_register_gradients);
    if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureComputedLOD &&
        (!use_computed_lod || instr.attributes.use_register_gradients)) {
      REX_ASSERT_ALWAYS();
      EmitTranslationError(
          "getCompTexLOD used with explicit LOD or gradients - contradicts "
          "MSDN",
          false);
      StoreResult(instr.result, const_float_vectors_0[used_result_component_count - 1]);
      return;
    }

    u32 fetch_constant_index = instr.operands[1].storage_index;
    u32 fetch_constant_word_0_index = 6 * fetch_constant_index;

    spv::Id sampler = spv::NoResult;
    spv::Id image_2d_array_or_cube_unsigned = spv::NoResult;
    spv::Id image_2d_array_or_cube_signed = spv::NoResult;
    spv::Id image_3d_unsigned = spv::NoResult;
    spv::Id image_3d_signed = spv::NoResult;
    if (instr.opcode != xenos::ucode::FetchOpcode::kGetTextureWeights) {
      bool bindings_set_up = true;
      size_t sampler_index = FindOrAddSamplerBinding(
          fetch_constant_index, instr.attributes.mag_filter, instr.attributes.min_filter,
          instr.opcode == xenos::ucode::FetchOpcode::kGetTextureComputedLOD
              ? xenos::TextureFilter::kLinear
              : instr.attributes.mip_filter,
          use_computed_lod ? instr.attributes.aniso_filter : xenos::AnisoFilter::kDisabled);
      xenos::FetchOpDimension dimension_2d_array_or_cube =
          instr.dimension == xenos::FetchOpDimension::k3DOrStacked ? xenos::FetchOpDimension::k2D
                                                                   : instr.dimension;
      size_t image_2d_array_or_cube_unsigned_index =
          FindOrAddTextureBinding(fetch_constant_index, dimension_2d_array_or_cube, false);
      size_t image_2d_array_or_cube_signed_index =
          FindOrAddTextureBinding(fetch_constant_index, dimension_2d_array_or_cube, true);
      if (sampler_index == SIZE_MAX || image_2d_array_or_cube_unsigned_index == SIZE_MAX ||
          image_2d_array_or_cube_signed_index == SIZE_MAX) {
        bindings_set_up = false;
      }
      size_t image_3d_unsigned_index = SIZE_MAX;
      size_t image_3d_signed_index = SIZE_MAX;
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        image_3d_unsigned_index = FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, false);
        image_3d_signed_index = FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, true);
        if (image_3d_unsigned_index == SIZE_MAX || image_3d_signed_index == SIZE_MAX) {
          bindings_set_up = false;
        }
      }
      if (!bindings_set_up) {
        StoreResult(instr.result, const_float_vectors_0[used_result_component_count - 1]);
        return;
      }
      sampler = builder_->createLoad(sampler_bindings_[sampler_index].variable, spv::NoPrecision);
      const TextureBinding& image_2d_array_or_cube_unsigned_binding =
          texture_bindings_[image_2d_array_or_cube_unsigned_index];
      image_2d_array_or_cube_unsigned =
          builder_->createLoad(image_2d_array_or_cube_unsigned_binding.variable, spv::NoPrecision);
      const TextureBinding& image_2d_array_or_cube_signed_binding =
          texture_bindings_[image_2d_array_or_cube_signed_index];
      image_2d_array_or_cube_signed =
          builder_->createLoad(image_2d_array_or_cube_signed_binding.variable, spv::NoPrecision);
      if (image_3d_unsigned_index != SIZE_MAX) {
        const TextureBinding& image_3d_unsigned_binding =
            texture_bindings_[image_3d_unsigned_index];
        image_3d_unsigned =
            builder_->createLoad(image_3d_unsigned_binding.variable, spv::NoPrecision);
      }
      if (image_3d_signed_index != SIZE_MAX) {
        const TextureBinding& image_3d_signed_binding = texture_bindings_[image_3d_signed_index];
        image_3d_signed = builder_->createLoad(image_3d_signed_binding.variable, spv::NoPrecision);
      }
    }

    f32 offset_values[3] = {};
    if (instr.opcode != xenos::ucode::FetchOpcode::kGetTextureComputedLOD) {
      constexpr f32 kRoundingOffset = 1.5f / 1024.0f;
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          offset_values[0] = instr.attributes.offset_x + kRoundingOffset;
          if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
            offset_values[0] -= 0.5f;
          }
          break;
        case xenos::FetchOpDimension::k2D:
          offset_values[0] = instr.attributes.offset_x + kRoundingOffset;
          offset_values[1] = instr.attributes.offset_y + kRoundingOffset;
          if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
            offset_values[0] -= 0.5f;
            offset_values[1] -= 0.5f;
          }
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          offset_values[0] = instr.attributes.offset_x + kRoundingOffset;
          offset_values[1] = instr.attributes.offset_y + kRoundingOffset;
          offset_values[2] = instr.attributes.offset_z + kRoundingOffset;
          if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
            offset_values[0] -= 0.5f;
            offset_values[1] -= 0.5f;
            offset_values[2] -= 0.5f;
          }
          break;
        case xenos::FetchOpDimension::kCube:
          offset_values[0] = instr.attributes.offset_x + kRoundingOffset;
          offset_values[1] = instr.attributes.offset_y + kRoundingOffset;
          if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
            offset_values[0] -= 0.5f;
            offset_values[1] -= 0.5f;
          } else {
            offset_values[2] = instr.attributes.offset_z;
          }
          break;
      }
    }
    u32 offsets_not_zero = 0b000;
    for (u32 i = 0; i < 3; ++i) {
      if (offset_values[i] != 0.0f) {
        offsets_not_zero |= 1 << i;
      }
    }

    u32 size_needed_components = 0b000;
    bool data_is_3d_needed = false;
    if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
      if (!instr.attributes.unnormalized_coordinates) {
        switch (instr.dimension) {
          case xenos::FetchOpDimension::k1D:
            size_needed_components |= used_result_nonzero_components & 0b0001;
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
          data_is_3d_needed = true;
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
      data_is_3d_needed = true;
    }
    spv::Id data_is_3d = spv::NoResult;
    if (data_is_3d_needed) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(const_int_0_);
      id_vector_temp_.push_back(
          builder_->makeIntConstant(int((fetch_constant_word_0_index + 5) >> 2)));
      id_vector_temp_.push_back(
          builder_->makeIntConstant(int((fetch_constant_word_0_index + 5) & 3)));
      spv::Id fetch_constant_word_5 = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_fetch_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      spv::Id data_dimension =
          builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, fetch_constant_word_5,
                                builder_->makeUintConstant(9), builder_->makeUintConstant(2));
      data_is_3d = builder_->createBinOp(
          spv::OpIEqual, type_bool, data_dimension,
          builder_->makeUintConstant(static_cast<unsigned int>(xenos::DataDimension::k3D)));
    }
    spv::Id size[3] = {};
    if (size_needed_components) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(const_int_0_);
      id_vector_temp_.push_back(
          builder_->makeIntConstant(int((fetch_constant_word_0_index + 2) >> 2)));
      id_vector_temp_.push_back(
          builder_->makeIntConstant(int((fetch_constant_word_0_index + 2) & 3)));
      spv::Id fetch_constant_word_2 = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_fetch_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D: {
          if (size_needed_components & 0b1) {
            size[0] = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2, const_uint_0_,
                builder_->makeUintConstant(xenos::kTexture1DMaxWidthLog2));
          }
          REX_ASSERT((size_needed_components & 0b110) == 0);
        } break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube: {
          if (size_needed_components & 0b1) {
            size[0] = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2, const_uint_0_,
                builder_->makeUintConstant(xenos::kTexture2DCubeMaxWidthHeightLog2));
          }
          if (size_needed_components & 0b10) {
            spv::Id width_height_bit_count =
                builder_->makeUintConstant(xenos::kTexture2DCubeMaxWidthHeightLog2);
            size[1] =
                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2,
                                      width_height_bit_count, width_height_bit_count);
          }
          REX_ASSERT((size_needed_components & 0b100) == 0);
        } break;
        case xenos::FetchOpDimension::k3DOrStacked: {
          if (size_needed_components & 0b1) {
            spv::Id size_3d = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2, const_uint_0_,
                builder_->makeUintConstant(xenos::kTexture3DMaxWidthHeightLog2));
            spv::Id size_2d = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2, const_uint_0_,
                builder_->makeUintConstant(xenos::kTexture2DCubeMaxWidthHeightLog2));
            REX_ASSERT(data_is_3d != spv::NoResult);
            size[0] = builder_->createTriOp(spv::OpSelect, type_uint, data_is_3d, size_3d, size_2d);
          }
          if (size_needed_components & 0b10) {
            spv::Id width_height_bit_count_3d =
                builder_->makeUintConstant(xenos::kTexture3DMaxWidthHeightLog2);
            spv::Id size_3d =
                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2,
                                      width_height_bit_count_3d, width_height_bit_count_3d);
            spv::Id width_height_bit_count_2d =
                builder_->makeUintConstant(xenos::kTexture2DCubeMaxWidthHeightLog2);
            spv::Id size_2d =
                builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2,
                                      width_height_bit_count_2d, width_height_bit_count_2d);
            REX_ASSERT(data_is_3d != spv::NoResult);
            size[1] = builder_->createTriOp(spv::OpSelect, type_uint, data_is_3d, size_3d, size_2d);
          }
          if (size_needed_components & 0b100) {
            spv::Id size_3d = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2,
                builder_->makeUintConstant(xenos::kTexture3DMaxWidthHeightLog2 * 2),
                builder_->makeUintConstant(xenos::kTexture3DMaxDepthLog2));
            spv::Id size_2d = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, fetch_constant_word_2,
                builder_->makeUintConstant(xenos::kTexture2DCubeMaxWidthHeightLog2 * 2),
                builder_->makeUintConstant(xenos::kTexture2DMaxStackDepthLog2));
            REX_ASSERT(data_is_3d != spv::NoResult);
            size[2] = builder_->createTriOp(spv::OpSelect, type_uint, data_is_3d, size_3d, size_2d);
          }
        } break;
      }
      {
        u32 size_remaining_components = size_needed_components;
        u32 size_component_index;
        while (rex::bit_scan_forward(size_remaining_components, &size_component_index)) {
          size_remaining_components &= ~(UINT32_C(1) << size_component_index);
          spv::Id& size_component_ref = size[size_component_index];
          size_component_ref = builder_->createBinOp(spv::OpIAdd, type_uint, size_component_ref,
                                                     builder_->makeUintConstant(1));
          size_component_ref =
              builder_->createUnaryOp(spv::OpConvertUToF, type_float, size_component_ref);
        }
      }
    }

    REX_ASSERT(!(instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights &&
                 (used_result_nonzero_components & 0b1000)));

    u32 coordinates_needed_components =
        instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights
            ? used_result_nonzero_components
            : ((UINT32_C(1) << xenos::GetFetchOpDimensionComponentCount(instr.dimension)) - 1);
    REX_ASSERT(coordinates_needed_components != 0);
    spv::Id coordinates_operand = GetOperandComponents(
        LoadOperandStorage(instr.operands[0]), instr.operands[0], coordinates_needed_components);
    spv::Id coordinates[] = {const_float_0, const_float_0, const_float_0};
    if (rex::BitCount(coordinates_needed_components) > 1) {
      u32 coordinates_remaining_components = coordinates_needed_components;
      u32 coordinate_component_index;
      u32 coordinate_operand_component_index = 0;
      while (rex::bit_scan_forward(coordinates_remaining_components, &coordinate_component_index)) {
        coordinates_remaining_components &= ~(UINT32_C(1) << coordinate_component_index);
        coordinates[coordinate_component_index] = builder_->createCompositeExtract(
            coordinates_operand, type_float, coordinate_operand_component_index++);
      }
    } else {
      u32 coordinate_component_index;
      rex::bit_scan_forward(coordinates_needed_components, &coordinate_component_index);
      coordinates[coordinate_component_index] = coordinates_operand;
    }
    spv::Id texture_resolution_scaled = spv::NoResult;
    u32 revert_resolution_scale_axes = static_cast<u32>(draw_resolution_scale_x_ > 1) |
                                       (static_cast<u32>(draw_resolution_scale_y_ > 1) << 1);
    spv::Id const_texture_resolution_scale[] = {spv::NoResult, spv::NoResult};
    spv::Id const_texture_resolution_scale_reciprocal[] = {spv::NoResult, spv::NoResult};
    if (revert_resolution_scale_axes) {
      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantTexturesResolutionScaled));
      spv::Id textures_resolution_scaled = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      texture_resolution_scaled = builder_->createBinOp(
          spv::OpINotEqual, type_bool,
          builder_->createBinOp(spv::OpBitwiseAnd, type_uint, textures_resolution_scaled,
                                builder_->makeUintConstant(UINT32_C(1) << fetch_constant_index)),
          const_uint_0_);
      if (revert_resolution_scale_axes & 0b001) {
        const_texture_resolution_scale[0] =
            builder_->makeFloatConstant(static_cast<f32>(draw_resolution_scale_x_));
        const_texture_resolution_scale_reciprocal[0] =
            builder_->makeFloatConstant(1.0f / static_cast<f32>(draw_resolution_scale_x_));
      }
      if (revert_resolution_scale_axes & 0b010) {
        const_texture_resolution_scale[1] =
            builder_->makeFloatConstant(static_cast<f32>(draw_resolution_scale_y_));
        const_texture_resolution_scale_reciprocal[1] =
            builder_->makeFloatConstant(1.0f / static_cast<f32>(draw_resolution_scale_y_));
      }
    }

    if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureWeights) {
      u32 resolution_scaled_result_components =
          used_result_nonzero_components & revert_resolution_scale_axes;
      u32 resolution_scaled_coord_components =
          instr.attributes.unnormalized_coordinates ? resolution_scaled_result_components : 0b000;
      u32 resolution_scaled_size_components =
          size_needed_components & resolution_scaled_result_components;
      if (texture_resolution_scaled != spv::NoResult &&
          (resolution_scaled_coord_components || resolution_scaled_size_components)) {
        for (u32 i = 0; i < 2; ++i) {
          if (resolution_scaled_coord_components & (UINT32_C(1) << i)) {
            coordinates[i] = builder_->createTriOp(
                spv::OpSelect, type_float, texture_resolution_scaled,
                builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, coordinates[i],
                                                   const_texture_resolution_scale[i]),
                coordinates[i]);
          }
          if (resolution_scaled_size_components & (UINT32_C(1) << i)) {
            REX_ASSERT(size[i] != spv::NoResult);
            size[i] = builder_->createTriOp(
                spv::OpSelect, type_float, texture_resolution_scaled,
                builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, size[i],
                                                   const_texture_resolution_scale[i]),
                size[i]);
          }
        }
      }

      u32 coordinates_remaining_components = coordinates_needed_components;
      u32 coordinate_component_index;
      while (rex::bit_scan_forward(coordinates_remaining_components, &coordinate_component_index)) {
        coordinates_remaining_components &= ~(UINT32_C(1) << coordinate_component_index);
        spv::Id result_component = coordinates[coordinate_component_index];
        if (!instr.attributes.unnormalized_coordinates) {
          spv::Id size_component = size[coordinate_component_index];
          REX_ASSERT(size_component != spv::NoResult);
          result_component = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float,
                                                                result_component, size_component);
        }
        f32 component_offset = offset_values[coordinate_component_index];
        if (component_offset != 0.0f) {
          result_component =
              builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, result_component,
                                                 builder_->makeFloatConstant(component_offset));
        }
        result_component = builder_->CreateUnaryBuiltinCall(type_float, ext_inst_glsl_std_450_,
                                                            GLSLstd450Fract, result_component);
        result[coordinate_component_index] = result_component;
      }
    } else {
      for (u32 i = 0; i <= static_cast<u32>(instr.dimension != xenos::FetchOpDimension::k1D); ++i) {
        spv::Id& coordinate_ref = coordinates[i];
        spv::Id component_offset = offset_values[i] != 0.0f
                                       ? builder_->makeFloatConstant(offset_values[i])
                                       : spv::NoResult;
        if (component_offset != spv::NoResult && texture_resolution_scaled != spv::NoResult &&
            (revert_resolution_scale_axes & (UINT32_C(1) << i))) {
          spv::Id component_offset_resolution_scaled =
              builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, component_offset,
                                                 const_texture_resolution_scale_reciprocal[i]);
          component_offset =
              builder_->createTriOp(spv::OpSelect, type_float, texture_resolution_scaled,
                                    component_offset_resolution_scaled, component_offset);
        }
        spv::Id size_component = size[i];
        if (instr.attributes.unnormalized_coordinates) {
          if (component_offset != spv::NoResult) {
            coordinate_ref = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float,
                                                                coordinate_ref, component_offset);
          }
          REX_ASSERT(size_component != spv::NoResult);
          coordinate_ref = builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float,
                                                              coordinate_ref, size_component);
        } else {
          if (component_offset != spv::NoResult) {
            REX_ASSERT(size_component != spv::NoResult);
            spv::Id component_offset_normalized = builder_->CreateNoContractionBinOp(
                spv::OpFDiv, type_float, component_offset, size_component);
            coordinate_ref = builder_->CreateNoContractionBinOp(
                spv::OpFAdd, type_float, coordinate_ref, component_offset_normalized);
          }
        }
      }
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        spv::Id& z_coordinate_ref = coordinates[2];
        spv::Id z_offset = offset_values[2] != 0.0f ? builder_->makeFloatConstant(offset_values[2])
                                                    : spv::NoResult;
        spv::Id z_size = size[2];
        if (instr.attributes.unnormalized_coordinates) {
          if (z_offset != spv::NoResult) {
            z_coordinate_ref = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float,
                                                                  z_coordinate_ref, z_offset);
          }
          REX_ASSERT(data_is_3d != spv::NoResult);
          SPIRVBuilder::IfBuilder if_data_is_3d(data_is_3d, spv::SelectionControlDontFlattenMask,
                                                *builder_);
          spv::Id z_3d;
          {
            REX_ASSERT(z_size != spv::NoResult);
            z_3d = builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float, z_coordinate_ref,
                                                      z_size);
          }
          if_data_is_3d.MakeEndIf();
          z_coordinate_ref = if_data_is_3d.CreateMergePhi(z_3d, z_coordinate_ref);
        } else {
          spv::Block& block_dimension_head = *builder_->getBuildPoint();
          spv::Block& block_dimension_merge = builder_->makeNewBlock();
          spv::Block* block_dimension_3d =
              z_offset != spv::NoResult ? &builder_->makeNewBlock() : nullptr;
          spv::Block& block_dimension_stacked = builder_->makeNewBlock();
          builder_->createSelectionMerge(&block_dimension_merge,
                                         spv::SelectionControlDontFlattenMask);
          REX_ASSERT(data_is_3d != spv::NoResult);
          builder_->createConditionalBranch(
              data_is_3d, block_dimension_3d ? block_dimension_3d : &block_dimension_merge,
              &block_dimension_stacked);
          spv::Id z_3d = z_coordinate_ref;
          if (block_dimension_3d) {
            builder_->setBuildPoint(block_dimension_3d);
            if (z_offset != spv::NoResult) {
              REX_ASSERT(z_size != spv::NoResult);
              spv::Id z_offset_normalized =
                  builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float, z_offset, z_size);
              z_3d = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, z_3d,
                                                        z_offset_normalized);
            }
            builder_->createBranch(&block_dimension_merge);
          }
          builder_->setBuildPoint(&block_dimension_stacked);
          spv::Id z_stacked = z_coordinate_ref;
          REX_ASSERT(z_size != spv::NoResult);
          z_stacked =
              builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, z_stacked, z_size);
          if (z_offset != spv::NoResult) {
            z_stacked =
                builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, z_stacked, z_offset);
          }
          builder_->createBranch(&block_dimension_merge);
          builder_->setBuildPoint(&block_dimension_merge);
          {
            std::unique_ptr<spv::Instruction> z_phi_op =
                std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float, spv::OpPhi);
            z_phi_op->addIdOperand(z_3d);
            z_phi_op->addIdOperand(
                (block_dimension_3d ? *block_dimension_3d : block_dimension_head).getId());
            z_phi_op->addIdOperand(z_stacked);
            z_phi_op->addIdOperand(block_dimension_stacked.getId());
            z_coordinate_ref = z_phi_op->getResultId();
            builder_->getBuildPoint()->addInstruction(std::move(z_phi_op));
          }
        }
      } else if (instr.dimension == xenos::FetchOpDimension::kCube) {
        spv::Id const_float_2 = builder_->makeFloatConstant(2.0f);
        spv::Id const_float_minus_3 = builder_->makeFloatConstant(-3.0f);
        for (u32 i = 0; i < 2; ++i) {
          coordinates[i] = builder_->CreateNoContractionBinOp(
              spv::OpFAdd, type_float,
              builder_->CreateNoContractionBinOp(spv::OpFMul, type_float, coordinates[i],
                                                 const_float_2),
              const_float_minus_3);
        }
        // OpConvertFToU is undefined for out-of-range values, so clamp the face index to 0...5.
        spv::Id face = coordinates[2];
        if (offset_values[2] != 0.0f) {
          face = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float, face,
                                                    builder_->makeFloatConstant(offset_values[2]));
        }
        face = builder_->createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder_->CreateTriBuiltinCall(type_float, ext_inst_glsl_std_450_, GLSLstd450NClamp,
                                           face, const_float_0, builder_->makeFloatConstant(5.0f)));
        spv::Id const_uint_1 = builder_->makeUintConstant(1);
        spv::Id face_axis =
            builder_->createBinOp(spv::OpShiftRightLogical, type_uint, face, const_uint_1);
        spv::Id face_is_negative = builder_->createBinOp(
            spv::OpINotEqual, type_bool,
            builder_->createBinOp(spv::OpBitwiseAnd, type_uint, face, const_uint_1), const_uint_0_);
        spv::Id face_sign = builder_->createTriOp(spv::OpSelect, type_float, face_is_negative,
                                                  builder_->makeFloatConstant(-1.0f),
                                                  builder_->makeFloatConstant(1.0f));
        spv::Id sc_negated =
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, coordinates[0]);
        spv::Id tc_negated =
            builder_->CreateNoContractionUnaryOp(spv::OpFNegate, type_float, coordinates[1]);
        spv::Block& block_ma_head = *builder_->getBuildPoint();
        spv::Block& block_ma_x = builder_->makeNewBlock();
        spv::Block& block_ma_y = builder_->makeNewBlock();
        spv::Block& block_ma_z = builder_->makeNewBlock();
        spv::Block& block_ma_merge = builder_->makeNewBlock();
        builder_->createSelectionMerge(&block_ma_merge, spv::SelectionControlMaskNone);
        {
          std::unique_ptr<spv::Instruction> ma_switch_op =
              std::make_unique<spv::Instruction>(spv::OpSwitch);
          ma_switch_op->addIdOperand(face_axis);
          ma_switch_op->addIdOperand(block_ma_z.getId());
          ma_switch_op->addImmediateOperand(0);
          ma_switch_op->addIdOperand(block_ma_x.getId());
          ma_switch_op->addImmediateOperand(1);
          ma_switch_op->addIdOperand(block_ma_y.getId());
          builder_->getBuildPoint()->addInstruction(std::move(ma_switch_op));
        }
        block_ma_x.addPredecessor(&block_ma_head);
        block_ma_y.addPredecessor(&block_ma_head);
        block_ma_z.addPredecessor(&block_ma_head);
        builder_->setBuildPoint(&block_ma_x);
        spv::Id ma_x_y = tc_negated;
        spv::Id ma_x_z = builder_->createTriOp(spv::OpSelect, type_float, face_is_negative,
                                               coordinates[0], sc_negated);
        builder_->createBranch(&block_ma_merge);
        builder_->setBuildPoint(&block_ma_y);
        spv::Id ma_y_x = coordinates[0];
        spv::Id ma_y_z = builder_->createTriOp(spv::OpSelect, type_float, face_is_negative,
                                               tc_negated, coordinates[1]);
        builder_->createBranch(&block_ma_merge);
        builder_->setBuildPoint(&block_ma_z);
        spv::Id ma_z_x = builder_->createTriOp(spv::OpSelect, type_float, face_is_negative,
                                               sc_negated, coordinates[0]);
        spv::Id ma_z_y = tc_negated;
        builder_->createBranch(&block_ma_merge);
        builder_->setBuildPoint(&block_ma_merge);
        {
          std::unique_ptr<spv::Instruction> x_phi_op =
              std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float, spv::OpPhi);
          x_phi_op->addIdOperand(face_sign);
          x_phi_op->addIdOperand(block_ma_x.getId());
          x_phi_op->addIdOperand(ma_y_x);
          x_phi_op->addIdOperand(block_ma_y.getId());
          x_phi_op->addIdOperand(ma_z_x);
          x_phi_op->addIdOperand(block_ma_z.getId());
          coordinates[0] = x_phi_op->getResultId();
          builder_->getBuildPoint()->addInstruction(std::move(x_phi_op));
        }
        {
          std::unique_ptr<spv::Instruction> y_phi_op =
              std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float, spv::OpPhi);
          y_phi_op->addIdOperand(ma_x_y);
          y_phi_op->addIdOperand(block_ma_x.getId());
          y_phi_op->addIdOperand(face_sign);
          y_phi_op->addIdOperand(block_ma_y.getId());
          y_phi_op->addIdOperand(ma_z_y);
          y_phi_op->addIdOperand(block_ma_z.getId());
          coordinates[1] = y_phi_op->getResultId();
          builder_->getBuildPoint()->addInstruction(std::move(y_phi_op));
        }
        {
          std::unique_ptr<spv::Instruction> z_phi_op =
              std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float, spv::OpPhi);
          z_phi_op->addIdOperand(ma_x_z);
          z_phi_op->addIdOperand(block_ma_x.getId());
          z_phi_op->addIdOperand(ma_y_z);
          z_phi_op->addIdOperand(block_ma_y.getId());
          z_phi_op->addIdOperand(face_sign);
          z_phi_op->addIdOperand(block_ma_z.getId());
          coordinates[2] = z_phi_op->getResultId();
          builder_->getBuildPoint()->addInstruction(std::move(z_phi_op));
        }
      }

      id_vector_temp_.clear();
      id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantTextureSwizzledSigns));
      id_vector_temp_.push_back(
          builder_->makeIntConstant(static_cast<int>(fetch_constant_index >> 4)));
      id_vector_temp_.push_back(
          builder_->makeIntConstant(static_cast<int>((fetch_constant_index >> 2) & 3)));
      spv::Id swizzled_signs_word = builder_->createLoad(
          builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                      id_vector_temp_),
          spv::NoPrecision);
      u32 swizzled_signs_word_offset = 8 * (fetch_constant_index & 3);

      spv::Builder::TextureParameters texture_parameters = {};

      if (instr.opcode == xenos::ucode::FetchOpcode::kGetTextureComputedLOD) {
        spv::Id swizzled_signs_all_signed = builder_->createBinOp(
            spv::OpIEqual, type_bool,
            builder_->createTriOp(spv::OpBitFieldUExtract, type_uint, swizzled_signs_word,
                                  builder_->makeUintConstant(swizzled_signs_word_offset),
                                  builder_->makeUintConstant(8)),
            builder_->makeUintConstant(std::to_underlying(xenos::TextureSign::kSigned) *
                                       0b01010101));

        // OpSampledImage must be in the same block as its use.
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          REX_ASSERT(data_is_3d != spv::NoResult);
          SPIRVBuilder::IfBuilder if_data_is_3d(data_is_3d, spv::SelectionControlDontFlattenMask,
                                                *builder_);
          spv::Id lod_3d;
          {
            id_vector_temp_.clear();
            for (u32 i = 0; i < 3; ++i) {
              id_vector_temp_.push_back(coordinates[i]);
            }
            texture_parameters.coords =
                builder_->createCompositeConstruct(type_float3, id_vector_temp_);
            lod_3d = QueryTextureLOD(texture_parameters, image_3d_unsigned, image_3d_signed,
                                     sampler, swizzled_signs_all_signed);
          }
          if_data_is_3d.MakeBeginElse();
          spv::Id lod_stacked;
          {
            id_vector_temp_.clear();
            for (u32 i = 0; i < 2; ++i) {
              id_vector_temp_.push_back(coordinates[i]);
            }
            texture_parameters.coords =
                builder_->createCompositeConstruct(type_float2, id_vector_temp_);
            lod_stacked =
                QueryTextureLOD(texture_parameters, image_2d_array_or_cube_unsigned,
                                image_2d_array_or_cube_signed, sampler, swizzled_signs_all_signed);
          }
          if_data_is_3d.MakeEndIf();
          result[0] = if_data_is_3d.CreateMergePhi(lod_3d, lod_stacked);
        } else {
          u32 lod_query_coordinate_component_count =
              instr.dimension == xenos::FetchOpDimension::kCube ? 3 : 2;
          id_vector_temp_.clear();
          for (u32 i = 0; i < lod_query_coordinate_component_count; ++i) {
            id_vector_temp_.push_back(coordinates[i]);
          }
          texture_parameters.coords = builder_->createCompositeConstruct(
              type_float_vectors[lod_query_coordinate_component_count - 1], id_vector_temp_);
          result[0] =
              QueryTextureLOD(texture_parameters, image_2d_array_or_cube_unsigned,
                              image_2d_array_or_cube_signed, sampler, swizzled_signs_all_signed);
        }
      } else {
        REX_ASSERT(instr.opcode == xenos::ucode::FetchOpcode::kTextureFetch);

        spv::Id swizzled_signs[4] = {};
        spv::Id result_is_signed[4] = {};
        spv::Id is_all_signed = spv::NoResult;
        spv::Id is_any_signed = spv::NoResult;
        spv::Id const_uint_2 = builder_->makeUintConstant(2);
        spv::Id const_uint_sign_signed =
            builder_->makeUintConstant(std::to_underlying(xenos::TextureSign::kSigned));
        {
          u32 result_remaining_components = used_result_nonzero_components;
          u32 result_component_index;
          while (rex::bit_scan_forward(result_remaining_components, &result_component_index)) {
            result_remaining_components &= ~(UINT32_C(1) << result_component_index);
            spv::Id result_component_sign = builder_->createTriOp(
                spv::OpBitFieldUExtract, type_uint, swizzled_signs_word,
                builder_->makeUintConstant(swizzled_signs_word_offset + 2 * result_component_index),
                const_uint_2);
            swizzled_signs[result_component_index] = result_component_sign;
            spv::Id is_component_signed = builder_->createBinOp(
                spv::OpIEqual, type_bool, result_component_sign, const_uint_sign_signed);
            result_is_signed[result_component_index] = is_component_signed;
            if (is_all_signed != spv::NoResult) {
              is_all_signed = builder_->createBinOp(spv::OpLogicalAnd, type_bool, is_all_signed,
                                                    is_component_signed);
            } else {
              is_all_signed = is_component_signed;
            }
            if (is_any_signed != spv::NoResult) {
              is_any_signed = builder_->createBinOp(spv::OpLogicalOr, type_bool, is_any_signed,
                                                    is_component_signed);
            } else {
              is_any_signed = is_component_signed;
            }
          }
        }
        spv::Id is_any_unsigned =
            builder_->createUnaryOp(spv::OpLogicalNot, type_bool, is_all_signed);

        id_vector_temp_.clear();
        id_vector_temp_.push_back(const_int_0_);
        id_vector_temp_.push_back(
            builder_->makeIntConstant(int((fetch_constant_word_0_index + 3) >> 2)));
        id_vector_temp_.push_back(
            builder_->makeIntConstant(int((fetch_constant_word_0_index + 3) & 3)));
        spv::Id fetch_constant_word_3 = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_fetch_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id fetch_constant_word_3_signed =
            builder_->createUnaryOp(spv::OpBitcast, type_int, fetch_constant_word_3);

        id_vector_temp_.clear();
        id_vector_temp_.push_back(const_int_0_);
        id_vector_temp_.push_back(
            builder_->makeIntConstant(int((fetch_constant_word_0_index + 4) >> 2)));
        id_vector_temp_.push_back(
            builder_->makeIntConstant(int((fetch_constant_word_0_index + 4) & 3)));
        spv::Id fetch_constant_word_4 = builder_->createLoad(
            builder_->createAccessChain(spv::StorageClassUniform, uniform_fetch_constants_,
                                        id_vector_temp_),
            spv::NoPrecision);
        spv::Id fetch_constant_word_4_signed =
            builder_->createUnaryOp(spv::OpBitcast, type_int, fetch_constant_word_4);

        spv::Id lod = builder_->CreateNoContractionBinOp(
            spv::OpFMul, type_float,
            builder_->createUnaryOp(
                spv::OpConvertSToF, type_float,
                builder_->createTriOp(spv::OpBitFieldSExtract, type_int,
                                      fetch_constant_word_4_signed, builder_->makeUintConstant(12),
                                      builder_->makeUintConstant(10))),
            builder_->makeFloatConstant(1.0f / 32.0f));
        if (instr.attributes.use_register_lod) {
          lod = builder_->CreateNoContractionBinOp(
              spv::OpFAdd, type_float, builder_->createLoad(var_main_tfetch_lod_, spv::NoPrecision),
              lod);
        }
        if (instr.attributes.lod_bias != 0.0f) {
          lod = builder_->CreateNoContractionBinOp(
              spv::OpFAdd, type_float, lod, builder_->makeFloatConstant(instr.attributes.lod_bias));
        }

        spv::Id gradients_h = spv::NoResult, gradients_v = spv::NoResult;
        if (use_computed_lod) {
          spv::Id lod_gradient_scale = builder_->CreateUnaryBuiltinCall(
              type_float, ext_inst_glsl_std_450_, GLSLstd450Exp2, lod);
          switch (instr.dimension) {
            case xenos::FetchOpDimension::k1D: {
              spv::Id gradient_h_1d, gradient_v_1d;
              if (instr.attributes.use_register_gradients) {
                id_vector_temp_.clear();
                id_vector_temp_.push_back(const_int_0_);
                gradient_h_1d = builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassFunction,
                                                var_main_tfetch_gradients_h_, id_vector_temp_),
                    spv::NoPrecision);
                gradient_v_1d = builder_->createLoad(
                    builder_->createAccessChain(spv::StorageClassFunction,
                                                var_main_tfetch_gradients_v_, id_vector_temp_),
                    spv::NoPrecision);
                if (instr.attributes.unnormalized_coordinates) {
                  REX_ASSERT(size[0] != spv::NoResult);
                  gradient_h_1d = builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float,
                                                                     gradient_h_1d, size[0]);
                  gradient_v_1d = builder_->CreateNoContractionBinOp(spv::OpFDiv, type_float,
                                                                     gradient_v_1d, size[0]);
                }
              } else {
                builder_->addCapability(spv::CapabilityDerivativeControl);
                gradient_h_1d =
                    builder_->createUnaryOp(spv::OpDPdxCoarse, type_float, coordinates[0]);
                gradient_v_1d =
                    builder_->createUnaryOp(spv::OpDPdyCoarse, type_float, coordinates[0]);
              }
              gradient_h_1d = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float,
                                                                 gradient_h_1d, lod_gradient_scale);
              gradient_v_1d = builder_->CreateNoContractionBinOp(spv::OpFMul, type_float,
                                                                 gradient_v_1d, lod_gradient_scale);
              id_vector_temp_.clear();
              id_vector_temp_.push_back(gradient_h_1d);
              id_vector_temp_.push_back(const_float_0);
              gradients_h = builder_->createCompositeConstruct(type_float2, id_vector_temp_);
              id_vector_temp_[0] = gradient_v_1d;
              gradients_v = builder_->createCompositeConstruct(type_float2, id_vector_temp_);
            } break;
            case xenos::FetchOpDimension::k2D: {
              if (instr.attributes.use_register_gradients) {
                for (u32 i = 0; i < 2; ++i) {
                  spv::Id register_gradient_3d = builder_->createLoad(
                      i ? var_main_tfetch_gradients_h_ : var_main_tfetch_gradients_v_,
                      spv::NoPrecision);
                  spv::Id register_gradient_x =
                      builder_->createCompositeExtract(register_gradient_3d, type_float, 0);
                  spv::Id register_gradient_y =
                      builder_->createCompositeExtract(register_gradient_3d, type_float, 1);
                  if (instr.attributes.unnormalized_coordinates) {
                    REX_ASSERT(size[0] != spv::NoResult);
                    register_gradient_x = builder_->CreateNoContractionBinOp(
                        spv::OpFDiv, type_float, register_gradient_x, size[0]);
                    REX_ASSERT(size[1] != spv::NoResult);
                    register_gradient_y = builder_->CreateNoContractionBinOp(
                        spv::OpFDiv, type_float, register_gradient_y, size[1]);
                  }
                  id_vector_temp_.clear();
                  id_vector_temp_.push_back(register_gradient_x);
                  id_vector_temp_.push_back(register_gradient_y);
                  (i ? gradients_v : gradients_h) =
                      builder_->createCompositeConstruct(type_float2, id_vector_temp_);
                }
              } else {
                id_vector_temp_.clear();
                for (u32 i = 0; i < 2; ++i) {
                  id_vector_temp_.push_back(coordinates[i]);
                }
                spv::Id gradient_coordinate_vector =
                    builder_->createCompositeConstruct(type_float2, id_vector_temp_);
                builder_->addCapability(spv::CapabilityDerivativeControl);
                gradients_h = builder_->createUnaryOp(spv::OpDPdxCoarse, type_float2,
                                                      gradient_coordinate_vector);
                gradients_v = builder_->createUnaryOp(spv::OpDPdyCoarse, type_float2,
                                                      gradient_coordinate_vector);
              }
              gradients_h = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float2, gradients_h, lod_gradient_scale);
              gradients_v = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float2, gradients_v, lod_gradient_scale);
            } break;
            case xenos::FetchOpDimension::k3DOrStacked: {
              if (instr.attributes.use_register_gradients) {
                gradients_h = builder_->createLoad(var_main_tfetch_gradients_h_, spv::NoPrecision);
                gradients_v = builder_->createLoad(var_main_tfetch_gradients_v_, spv::NoPrecision);
                if (instr.attributes.unnormalized_coordinates) {
                  for (u32 i = 0; i < 2; ++i) {
                    spv::Id& gradient_ref = i ? gradients_v : gradients_h;
                    id_vector_temp_.clear();
                    for (u32 j = 0; j < 3; ++j) {
                      REX_ASSERT(size[j] != spv::NoResult);
                      id_vector_temp_.push_back(builder_->CreateNoContractionBinOp(
                          spv::OpFDiv, type_float,
                          builder_->createCompositeExtract(gradient_ref, type_float, j), size[j]));
                    }
                    gradient_ref = builder_->createCompositeConstruct(type_float3, id_vector_temp_);
                  }
                }
              } else {
                id_vector_temp_.clear();
                for (u32 i = 0; i < 3; ++i) {
                  id_vector_temp_.push_back(coordinates[i]);
                }
                spv::Id gradient_coordinate_vector =
                    builder_->createCompositeConstruct(type_float3, id_vector_temp_);
                builder_->addCapability(spv::CapabilityDerivativeControl);
                gradients_h = builder_->createUnaryOp(spv::OpDPdxCoarse, type_float3,
                                                      gradient_coordinate_vector);
                gradients_v = builder_->createUnaryOp(spv::OpDPdyCoarse, type_float3,
                                                      gradient_coordinate_vector);
              }
              gradients_h = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float3, gradients_h, lod_gradient_scale);
              gradients_v = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float3, gradients_v, lod_gradient_scale);
            } break;
            case xenos::FetchOpDimension::kCube: {
              if (instr.attributes.use_register_gradients) {
                gradients_h = builder_->createLoad(var_main_tfetch_gradients_h_, spv::NoPrecision);
                gradients_v = builder_->createLoad(var_main_tfetch_gradients_v_, spv::NoPrecision);
              } else {
                id_vector_temp_.clear();
                for (u32 i = 0; i < 3; ++i) {
                  id_vector_temp_.push_back(coordinates[i]);
                }
                spv::Id gradient_coordinate_vector =
                    builder_->createCompositeConstruct(type_float3, id_vector_temp_);
                builder_->addCapability(spv::CapabilityDerivativeControl);
                gradients_h = builder_->createUnaryOp(spv::OpDPdxCoarse, type_float3,
                                                      gradient_coordinate_vector);
                gradients_v = builder_->createUnaryOp(spv::OpDPdyCoarse, type_float3,
                                                      gradient_coordinate_vector);
              }
              gradients_h = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float3, gradients_h, lod_gradient_scale);
              gradients_v = builder_->CreateNoContractionBinOp(
                  spv::OpVectorTimesScalar, type_float3, gradients_v, lod_gradient_scale);
            } break;
          }
        }

        spv::ImageOperandsMask image_operands_mask =
            use_computed_lod ? spv::ImageOperandsGradMask : spv::ImageOperandsLodMask;
        spv::Id sample_result_unsigned, sample_result_signed;
        if (!use_computed_lod) {
          texture_parameters.lod = lod;
        }
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          REX_ASSERT(data_is_3d != spv::NoResult);
          SPIRVBuilder::IfBuilder if_data_is_3d(data_is_3d, spv::SelectionControlDontFlattenMask,
                                                *builder_);
          spv::Id sample_result_unsigned_3d, sample_result_signed_3d;
          {
            if (use_computed_lod) {
              texture_parameters.gradX = gradients_h;
              texture_parameters.gradY = gradients_v;
            }
            id_vector_temp_.clear();
            for (u32 i = 0; i < 3; ++i) {
              id_vector_temp_.push_back(coordinates[i]);
            }
            texture_parameters.coords =
                builder_->createCompositeConstruct(type_float3, id_vector_temp_);
            SampleTexture(texture_parameters, image_operands_mask, image_3d_unsigned,
                          image_3d_signed, sampler, is_any_unsigned, is_any_signed,
                          sample_result_unsigned_3d, sample_result_signed_3d);
          }
          if_data_is_3d.MakeBeginElse();
          spv::Id sample_result_unsigned_stacked, sample_result_signed_stacked;
          {
            if (use_computed_lod) {
              uint_vector_temp_.clear();
              uint_vector_temp_.push_back(0);
              uint_vector_temp_.push_back(1);
              texture_parameters.gradX = builder_->createRvalueSwizzle(
                  spv::NoPrecision, type_float2, gradients_h, uint_vector_temp_);
              texture_parameters.gradY = builder_->createRvalueSwizzle(
                  spv::NoPrecision, type_float2, gradients_v, uint_vector_temp_);
            }
            bool vol_mag_filter_is_fetch_const =
                instr.attributes.vol_mag_filter == xenos::TextureFilter::kUseFetchConst;
            bool vol_min_filter_is_fetch_const =
                instr.attributes.vol_min_filter == xenos::TextureFilter::kUseFetchConst;
            bool vol_mag_filter_is_linear =
                instr.attributes.vol_mag_filter == xenos::TextureFilter::kLinear;
            bool vol_min_filter_is_linear =
                instr.attributes.vol_min_filter == xenos::TextureFilter::kLinear;
            spv::Id vol_filter_is_linear = spv::NoResult;
            if (use_computed_lod &&
                (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const ||
                 vol_mag_filter_is_linear != vol_min_filter_is_linear)) {
              spv::Id layer_max_gradient = builder_->CreateBinBuiltinCall(
                  type_float, ext_inst_glsl_std_450_, GLSLstd450NMax,
                  builder_->createCompositeExtract(gradients_h, type_float, 2),
                  builder_->createCompositeExtract(gradients_v, type_float, 2));
              if (!instr.attributes.unnormalized_coordinates) {
                REX_ASSERT(size[2] != spv::NoResult);
                layer_max_gradient = builder_->CreateNoContractionBinOp(
                    spv::OpFMul, type_float, layer_max_gradient, size[2]);
              }
              spv::Id is_minifying_z =
                  builder_->createBinOp(spv::OpFOrdLessThan, type_bool, layer_max_gradient,
                                        builder_->makeFloatConstant(1.0f));
              spv::Id vol_mag_filter_is_linear_loaded =
                  vol_mag_filter_is_fetch_const
                      ? builder_->createBinOp(
                            spv::OpINotEqual, type_bool,
                            builder_->createBinOp(spv::OpBitwiseAnd, type_uint,
                                                  fetch_constant_word_4,
                                                  builder_->makeUintConstant(UINT32_C(1) << 0)),
                            const_uint_0_)
                      : builder_->makeBoolConstant(vol_mag_filter_is_linear);
              spv::Id vol_min_filter_is_linear_loaded =
                  vol_min_filter_is_fetch_const
                      ? builder_->createBinOp(
                            spv::OpINotEqual, type_bool,
                            builder_->createBinOp(spv::OpBitwiseAnd, type_uint,
                                                  fetch_constant_word_4,
                                                  builder_->makeUintConstant(UINT32_C(1) << 1)),
                            const_uint_0_)
                      : builder_->makeBoolConstant(vol_min_filter_is_linear);
              vol_filter_is_linear = builder_->createTriOp(spv::OpSelect, type_bool, is_minifying_z,
                                                           vol_min_filter_is_linear_loaded,
                                                           vol_mag_filter_is_linear_loaded);
            } else {
              if (vol_mag_filter_is_fetch_const) {
                vol_filter_is_linear = builder_->createBinOp(
                    spv::OpINotEqual, type_bool,
                    builder_->createBinOp(spv::OpBitwiseAnd, type_uint, fetch_constant_word_4,
                                          builder_->makeUintConstant(UINT32_C(1) << 0)),
                    const_uint_0_);
              }
            }
            spv::Id layer_coordinate = coordinates[2];
            if (vol_filter_is_linear != spv::NoResult) {
              layer_coordinate = builder_->createTriOp(
                  spv::OpSelect, type_float, vol_filter_is_linear,
                  builder_->CreateNoContractionBinOp(spv::OpFSub, type_float, layer_coordinate,
                                                     builder_->makeFloatConstant(0.5f)),
                  layer_coordinate);
            } else if (vol_mag_filter_is_linear) {
              layer_coordinate = builder_->CreateNoContractionBinOp(
                  spv::OpFSub, type_float, layer_coordinate, builder_->makeFloatConstant(0.5f));
            }
            spv::Id layer_0_coordinate = builder_->CreateUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450_, GLSLstd450Floor, layer_coordinate);
            id_vector_temp_.clear();
            id_vector_temp_.push_back(coordinates[0]);
            id_vector_temp_.push_back(coordinates[1]);
            id_vector_temp_.push_back(layer_0_coordinate);
            texture_parameters.coords =
                builder_->createCompositeConstruct(type_float3, id_vector_temp_);
            SampleTexture(texture_parameters, image_operands_mask, image_2d_array_or_cube_unsigned,
                          image_2d_array_or_cube_signed, sampler, is_any_unsigned, is_any_signed,
                          sample_result_unsigned_stacked, sample_result_signed_stacked);
            if (vol_filter_is_linear != spv::NoResult || vol_mag_filter_is_linear) {
              spv::Block& block_z_head = *builder_->getBuildPoint();
              spv::Block& block_z_linear =
                  (vol_filter_is_linear != spv::NoResult) ? builder_->makeNewBlock() : block_z_head;
              spv::Block& block_z_merge =
                  (vol_filter_is_linear != spv::NoResult) ? builder_->makeNewBlock() : block_z_head;
              if (vol_filter_is_linear != spv::NoResult) {
                builder_->createSelectionMerge(&block_z_merge,
                                               spv::SelectionControlDontFlattenMask);
                builder_->createConditionalBranch(vol_filter_is_linear, &block_z_linear,
                                                  &block_z_merge);
                builder_->setBuildPoint(&block_z_linear);
              }
              spv::Id layer_1_coordinate = builder_->createBinOp(
                  spv::OpFAdd, type_float, layer_0_coordinate, builder_->makeFloatConstant(1.0f));
              id_vector_temp_.clear();
              id_vector_temp_.push_back(coordinates[0]);
              id_vector_temp_.push_back(coordinates[1]);
              id_vector_temp_.push_back(layer_1_coordinate);
              texture_parameters.coords =
                  builder_->createCompositeConstruct(type_float3, id_vector_temp_);
              spv::Id layer_lerp_factor = builder_->CreateUnaryBuiltinCall(
                  type_float, ext_inst_glsl_std_450_, GLSLstd450Fract, layer_coordinate);
              spv::Id sample_result_unsigned_stacked_filtered;
              spv::Id sample_result_signed_stacked_filtered;
              SampleTexture(texture_parameters, image_operands_mask,
                            image_2d_array_or_cube_unsigned, image_2d_array_or_cube_signed, sampler,
                            is_any_unsigned, is_any_signed, sample_result_unsigned_stacked_filtered,
                            sample_result_signed_stacked_filtered, layer_lerp_factor,
                            sample_result_unsigned_stacked, sample_result_signed_stacked);
              if (vol_filter_is_linear != spv::NoResult) {
                // Take the build point after the SampleTexture call for the phi.
                spv::Block& block_z_linear_end = *builder_->getBuildPoint();
                builder_->createBranch(&block_z_merge);
                builder_->setBuildPoint(&block_z_merge);
                {
                  std::unique_ptr<spv::Instruction> filter_phi_op =
                      std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4,
                                                         spv::OpPhi);
                  filter_phi_op->addIdOperand(sample_result_unsigned_stacked_filtered);
                  filter_phi_op->addIdOperand(block_z_linear_end.getId());
                  filter_phi_op->addIdOperand(sample_result_unsigned_stacked);
                  filter_phi_op->addIdOperand(block_z_head.getId());
                  sample_result_unsigned_stacked = filter_phi_op->getResultId();
                  builder_->getBuildPoint()->addInstruction(std::move(filter_phi_op));
                }
                {
                  std::unique_ptr<spv::Instruction> filter_phi_op =
                      std::make_unique<spv::Instruction>(builder_->getUniqueId(), type_float4,
                                                         spv::OpPhi);
                  filter_phi_op->addIdOperand(sample_result_signed_stacked_filtered);
                  filter_phi_op->addIdOperand(block_z_linear_end.getId());
                  filter_phi_op->addIdOperand(sample_result_signed_stacked);
                  filter_phi_op->addIdOperand(block_z_head.getId());
                  sample_result_signed_stacked = filter_phi_op->getResultId();
                  builder_->getBuildPoint()->addInstruction(std::move(filter_phi_op));
                }
              } else {
                sample_result_unsigned_stacked = sample_result_unsigned_stacked_filtered;
                sample_result_signed_stacked = sample_result_signed_stacked_filtered;
              }
            }
          }
          if_data_is_3d.MakeEndIf();

          sample_result_unsigned = if_data_is_3d.CreateMergePhi(sample_result_unsigned_3d,
                                                                sample_result_unsigned_stacked);
          sample_result_signed =
              if_data_is_3d.CreateMergePhi(sample_result_signed_3d, sample_result_signed_stacked);
        } else {
          if (use_computed_lod) {
            texture_parameters.gradX = gradients_h;
            texture_parameters.gradY = gradients_v;
          }
          id_vector_temp_.clear();
          for (u32 i = 0; i < 3; ++i) {
            id_vector_temp_.push_back(coordinates[i]);
          }
          texture_parameters.coords =
              builder_->createCompositeConstruct(type_float3, id_vector_temp_);
          SampleTexture(texture_parameters, image_operands_mask, image_2d_array_or_cube_unsigned,
                        image_2d_array_or_cube_signed, sampler, is_any_unsigned, is_any_signed,
                        sample_result_unsigned, sample_result_signed);
        }

        if (!features_.image_view_format_swizzle) {
          id_vector_temp_.clear();
          id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantTextureSwizzles));
          id_vector_temp_.push_back(
              builder_->makeIntConstant(static_cast<int>(fetch_constant_index >> 3)));
          id_vector_temp_.push_back(
              builder_->makeIntConstant(static_cast<int>((fetch_constant_index >> 1) & 3)));
          spv::Id swizzle_word = builder_->createLoad(
              builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_,
                                          id_vector_temp_),
              spv::NoPrecision);
          u32 swizzle_word_offset = 3 * 4 * (fetch_constant_index & 1);
          spv::Id const_float_1 = builder_->makeFloatConstant(1.0f);
          u32 result_remaining_components = used_result_nonzero_components;
          u32 result_component_index;
          while (rex::bit_scan_forward(result_remaining_components, &result_component_index)) {
            result_remaining_components &= ~(UINT32_C(1) << result_component_index);
            u32 swizzle_bit_0_value = UINT32_C(1)
                                      << (swizzle_word_offset + 3 * result_component_index);
            spv::Id swizzle_bit_0 = builder_->createBinOp(
                spv::OpINotEqual, type_bool,
                builder_->createBinOp(spv::OpBitwiseAnd, type_uint, swizzle_word,
                                      builder_->makeUintConstant(swizzle_bit_0_value)),
                const_uint_0_);
            spv::Id swizzle_bit_2 = builder_->createBinOp(
                spv::OpINotEqual, type_bool,
                builder_->createBinOp(spv::OpBitwiseAnd, type_uint, swizzle_word,
                                      builder_->makeUintConstant(swizzle_bit_0_value << 2)),
                const_uint_0_);
            SPIRVBuilder::IfBuilder if_swizzle_constant(
                swizzle_bit_2, spv::SelectionControlDontFlattenMask, *builder_);
            spv::Id swizzle_result_constant;
            {
              swizzle_result_constant = builder_->createTriOp(
                  spv::OpSelect, type_float, swizzle_bit_0, const_float_1, const_float_0);
            }
            if_swizzle_constant.MakeBeginElse();
            spv::Id swizzle_result_component;
            {
              spv::Id swizzle_sample_result = builder_->createTriOp(
                  spv::OpSelect, type_float4,
                  builder_->smearScalar(spv::NoPrecision, result_is_signed[result_component_index],
                                        type_bool4),
                  sample_result_signed, sample_result_unsigned);
              spv::Id swizzle_x_or_y = builder_->createTriOp(
                  spv::OpSelect, type_float, swizzle_bit_0,
                  builder_->createCompositeExtract(swizzle_sample_result, type_float, 1),
                  builder_->createCompositeExtract(swizzle_sample_result, type_float, 0));
              spv::Id swizzle_z_or_w = builder_->createTriOp(
                  spv::OpSelect, type_float, swizzle_bit_0,
                  builder_->createCompositeExtract(swizzle_sample_result, type_float, 3),
                  builder_->createCompositeExtract(swizzle_sample_result, type_float, 2));
              spv::Id swizzle_bit_1 = builder_->createBinOp(
                  spv::OpINotEqual, type_bool,
                  builder_->createBinOp(spv::OpBitwiseAnd, type_uint, swizzle_word,
                                        builder_->makeUintConstant(swizzle_bit_0_value << 1)),
                  const_uint_0_);
              swizzle_result_component = builder_->createTriOp(
                  spv::OpSelect, type_float, swizzle_bit_1, swizzle_z_or_w, swizzle_x_or_y);
            }
            if_swizzle_constant.MakeEndIf();
            result[result_component_index] = if_swizzle_constant.CreateMergePhi(
                swizzle_result_constant, swizzle_result_component);
          }
        }

        spv::Id const_float_2 = builder_->makeFloatConstant(2.0f);
        spv::Id const_float_minus_1 = builder_->makeFloatConstant(-1.0f);
        {
          u32 result_remaining_components = used_result_nonzero_components;
          u32 result_component_index;
          while (rex::bit_scan_forward(result_remaining_components, &result_component_index)) {
            result_remaining_components &= ~(UINT32_C(1) << result_component_index);
            spv::Id sample_result_component_unsigned =
                features_.image_view_format_swizzle
                    ? builder_->createCompositeExtract(sample_result_unsigned, type_float,
                                                       result_component_index)
                    : result[result_component_index];
            spv::Block& block_sign_head = *builder_->getBuildPoint();
            spv::Block* block_sign_signed =
                features_.image_view_format_swizzle ? &builder_->makeNewBlock() : nullptr;
            spv::Block& block_sign_unsigned_biased = builder_->makeNewBlock();
            spv::Block& block_sign_gamma_start = builder_->makeNewBlock();
            spv::Block& block_sign_merge = builder_->makeNewBlock();
            builder_->createSelectionMerge(&block_sign_merge, spv::SelectionControlDontFlattenMask);
            {
              std::unique_ptr<spv::Instruction> sign_switch_op =
                  std::make_unique<spv::Instruction>(spv::OpSwitch);
              sign_switch_op->addIdOperand(swizzled_signs[result_component_index]);
              sign_switch_op->addIdOperand(block_sign_merge.getId());
              if (block_sign_signed) {
                sign_switch_op->addImmediateOperand(
                    std::to_underlying(xenos::TextureSign::kSigned));
                sign_switch_op->addIdOperand(block_sign_signed->getId());
              }
              sign_switch_op->addImmediateOperand(
                  std::to_underlying(xenos::TextureSign::kUnsignedBiased));
              sign_switch_op->addIdOperand(block_sign_unsigned_biased.getId());
              sign_switch_op->addImmediateOperand(std::to_underlying(xenos::TextureSign::kGamma));
              sign_switch_op->addIdOperand(block_sign_gamma_start.getId());
              builder_->getBuildPoint()->addInstruction(std::move(sign_switch_op));
            }
            if (block_sign_signed) {
              block_sign_signed->addPredecessor(&block_sign_head);
            }
            block_sign_unsigned_biased.addPredecessor(&block_sign_head);
            block_sign_gamma_start.addPredecessor(&block_sign_head);
            block_sign_merge.addPredecessor(&block_sign_head);
            spv::Id sample_result_component_signed = sample_result_component_unsigned;
            if (block_sign_signed) {
              builder_->setBuildPoint(block_sign_signed);
              sample_result_component_signed = builder_->createCompositeExtract(
                  sample_result_signed, type_float, result_component_index);
              builder_->createBranch(&block_sign_merge);
            }
            builder_->setBuildPoint(&block_sign_unsigned_biased);
            spv::Id sample_result_component_unsigned_biased = builder_->CreateNoContractionBinOp(
                spv::OpFMul, type_float, sample_result_component_unsigned, const_float_2);
            sample_result_component_unsigned_biased = builder_->CreateNoContractionBinOp(
                spv::OpFAdd, type_float, sample_result_component_unsigned_biased,
                const_float_minus_1);
            builder_->createBranch(&block_sign_merge);
            builder_->setBuildPoint(&block_sign_gamma_start);
            spv::Id sample_result_component_gamma =
                PWLGammaToLinear(sample_result_component_unsigned, false);
            // Take the build point after PWLGammaToLinear for the phi.
            spv::Block& block_sign_gamma_end = *builder_->getBuildPoint();
            builder_->createBranch(&block_sign_merge);
            builder_->setBuildPoint(&block_sign_merge);
            {
              std::unique_ptr<spv::Instruction> sign_phi_op = std::make_unique<spv::Instruction>(
                  builder_->getUniqueId(), type_float, spv::OpPhi);
              if (block_sign_signed) {
                sign_phi_op->addIdOperand(sample_result_component_signed);
                sign_phi_op->addIdOperand(block_sign_signed->getId());
              }
              sign_phi_op->addIdOperand(sample_result_component_unsigned_biased);
              sign_phi_op->addIdOperand(block_sign_unsigned_biased.getId());
              sign_phi_op->addIdOperand(sample_result_component_gamma);
              sign_phi_op->addIdOperand(block_sign_gamma_end.getId());
              sign_phi_op->addIdOperand(sample_result_component_unsigned);
              sign_phi_op->addIdOperand(block_sign_head.getId());
              result[result_component_index] = sign_phi_op->getResultId();
              builder_->getBuildPoint()->addInstruction(std::move(sign_phi_op));
            }
          }
        }

        spv::Id result_exponent_bias = builder_->CreateBinBuiltinCall(
            type_float, ext_inst_glsl_std_450_, GLSLstd450Ldexp, const_float_1,
            builder_->createTriOp(spv::OpBitFieldSExtract, type_int, fetch_constant_word_3_signed,
                                  builder_->makeUintConstant(13), builder_->makeUintConstant(6)));
        {
          u32 result_remaining_components = used_result_nonzero_components;
          u32 result_component_index;
          while (rex::bit_scan_forward(result_remaining_components, &result_component_index)) {
            result_remaining_components &= ~(UINT32_C(1) << result_component_index);
            result[result_component_index] = builder_->CreateNoContractionBinOp(
                spv::OpFMul, type_float, result[result_component_index], result_exponent_bias);
          }
        }
      }
    }
  }

  spv::Id result_vector;
  if (used_result_component_count > 1) {
    id_vector_temp_.clear();
    u32 result_components_remaining = used_result_components;
    u32 result_component_index;
    while (rex::bit_scan_forward(result_components_remaining, &result_component_index)) {
      result_components_remaining &= ~(UINT32_C(1) << result_component_index);
      id_vector_temp_.push_back(result[result_component_index]);
    }
    result_vector = builder_->createCompositeConstruct(
        type_float_vectors[used_result_component_count - 1], id_vector_temp_);
  } else {
    u32 result_component_index;
    rex::bit_scan_forward(used_result_components, &result_component_index);
    result_vector = result[result_component_index];
  }
  StoreResult(instr.result, result_vector);
}

size_t SPIRVShaderTranslator::FindOrAddTextureBinding(u32 fetch_constant,
                                                      xenos::FetchOpDimension dimension,
                                                      bool is_signed) {
  if (dimension == xenos::FetchOpDimension::k1D) {
    dimension = xenos::FetchOpDimension::k2D;
  }
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    const TextureBinding& texture_binding = texture_bindings_[i];
    if (texture_binding.fetch_constant == fetch_constant &&
        texture_binding.dimension == dimension && texture_binding.is_signed == is_signed) {
      return i;
    }
  }
  size_t new_texture_binding_index = texture_bindings_.size();
  TextureBinding& new_texture_binding = texture_bindings_.emplace_back();
  new_texture_binding.fetch_constant = fetch_constant;
  new_texture_binding.dimension = dimension;
  new_texture_binding.is_signed = is_signed;
  spv::Dim type_dimension;
  bool is_array;
  const char* dimension_name;
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      type_dimension = spv::Dim3D;
      is_array = false;
      dimension_name = "3d";
      break;
    case xenos::FetchOpDimension::kCube:
      type_dimension = spv::DimCube;
      is_array = false;
      dimension_name = "cube";
      break;
    default:
      type_dimension = spv::Dim2D;
      is_array = true;
      dimension_name = "2d";
  }
  new_texture_binding.variable = builder_->createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder_->makeImageType(type_float, type_dimension, false, is_array, false, 1,
                              spv::ImageFormatUnknown),
      std::format("xe_texture{}_{}_{}", fetch_constant, dimension_name, is_signed ? 's' : 'u')
          .c_str());
  builder_->addDecoration(
      new_texture_binding.variable, spv::DecorationDescriptorSet,
      int(IsVertexShader() ? kDescriptorSetTexturesVertex : kDescriptorSetTexturesPixel));
  builder_->addDecoration(new_texture_binding.variable, spv::DecorationBinding,
                          int(new_texture_binding_index));
  if (features_.spirv_version >= spv::Spv_1_4) {
    main_interface_.push_back(new_texture_binding.variable);
  }
  return new_texture_binding_index;
}

size_t SPIRVShaderTranslator::FindOrAddSamplerBinding(u32 fetch_constant,
                                                      xenos::TextureFilter mag_filter,
                                                      xenos::TextureFilter min_filter,
                                                      xenos::TextureFilter mip_filter,
                                                      xenos::AnisoFilter aniso_filter) {
  if (aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax161);
  }
  for (size_t i = 0; i < sampler_bindings_.size(); ++i) {
    const SamplerBinding& sampler_binding = sampler_bindings_[i];
    if (sampler_binding.fetch_constant == fetch_constant &&
        sampler_binding.mag_filter == mag_filter && sampler_binding.min_filter == min_filter &&
        sampler_binding.mip_filter == mip_filter && sampler_binding.aniso_filter == aniso_filter) {
      return i;
    }
  }
  size_t new_sampler_binding_index = sampler_bindings_.size();
  SamplerBinding& new_sampler_binding = sampler_bindings_.emplace_back();
  new_sampler_binding.fetch_constant = fetch_constant;
  new_sampler_binding.mag_filter = mag_filter;
  new_sampler_binding.min_filter = min_filter;
  new_sampler_binding.mip_filter = mip_filter;
  new_sampler_binding.aniso_filter = aniso_filter;
  std::ostringstream name;
  static const char kFilterSuffixes[] = {'p', 'l', 'b', 'f'};
  name << "xe_sampler" << fetch_constant << '_' << kFilterSuffixes[static_cast<u32>(mag_filter)]
       << kFilterSuffixes[static_cast<u32>(min_filter)]
       << kFilterSuffixes[static_cast<u32>(mip_filter)];
  if (aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    if (aniso_filter == xenos::AnisoFilter::kDisabled) {
      name << "_a0";
    } else {
      name << "_a" << (UINT32_C(1) << (static_cast<u32>(aniso_filter) - 1));
    }
  }
  new_sampler_binding.variable =
      builder_->createVariable(spv::NoPrecision, spv::StorageClassUniformConstant,
                               builder_->makeSamplerType(), name.str().c_str());
  builder_->addDecoration(
      new_sampler_binding.variable, spv::DecorationDescriptorSet,
      int(IsVertexShader() ? kDescriptorSetTexturesVertex : kDescriptorSetTexturesPixel));
  if (features_.spirv_version >= spv::Spv_1_4) {
    main_interface_.push_back(new_sampler_binding.variable);
  }
  return new_sampler_binding_index;
}

void SPIRVShaderTranslator::SampleTexture(spv::Builder::TextureParameters& texture_parameters,
                                          spv::ImageOperandsMask image_operands_mask,
                                          spv::Id image_unsigned, spv::Id image_signed,
                                          spv::Id sampler, spv::Id is_any_unsigned,
                                          spv::Id is_any_signed, spv::Id& result_unsigned_out,
                                          spv::Id& result_signed_out, spv::Id lerp_factor,
                                          spv::Id lerp_first_unsigned, spv::Id lerp_first_signed) {
  for (u32 i = 0; i < 2; ++i) {
    SPIRVBuilder::IfBuilder sign_if(i ? is_any_signed : is_any_unsigned,
                                    spv::SelectionControlDontFlattenMask, *builder_);
    spv::Id sign_result;
    {
      spv::Id image = i ? image_signed : image_unsigned;
      // OpSampledImage must be in the same block as its use.
      texture_parameters.sampler = builder_->createBinOp(
          spv::OpSampledImage, builder_->makeSampledImageType(builder_->getTypeId(image)), image,
          sampler);
      sign_result =
          builder_->createTextureCall(spv::NoPrecision, type_float4, false, false, false, false,
                                      false, texture_parameters, image_operands_mask);
      if (lerp_factor != spv::NoResult) {
        spv::Id lerp_first = i ? lerp_first_signed : lerp_first_unsigned;
        if (lerp_first != spv::NoResult) {
          spv::Id lerp_difference = builder_->CreateNoContractionBinOp(
              spv::OpVectorTimesScalar, type_float4,
              builder_->CreateNoContractionBinOp(spv::OpFSub, type_float4, sign_result, lerp_first),
              lerp_factor);
          sign_result = builder_->CreateNoContractionBinOp(spv::OpFAdd, type_float4, sign_result,
                                                           lerp_difference);
        }
      }
    }
    sign_if.MakeEndIf();
    (i ? result_signed_out : result_unsigned_out) =
        sign_if.CreateMergePhi(sign_result, const_float4_0);
  }
}

spv::Id SPIRVShaderTranslator::QueryTextureLOD(spv::Builder::TextureParameters& texture_parameters,
                                               spv::Id image_unsigned, spv::Id image_signed,
                                               spv::Id sampler, spv::Id is_all_signed) {
  // OpSampledImage must be in the same block as its use.
  SPIRVBuilder::IfBuilder if_signed(is_all_signed, spv::SelectionControlDontFlattenMask, *builder_);
  spv::Id lod_signed;
  {
    texture_parameters.sampler = builder_->createBinOp(
        spv::OpSampledImage, builder_->makeSampledImageType(builder_->getTypeId(image_signed)),
        image_signed, sampler);
    lod_signed = builder_->createCompositeExtract(
        builder_->createTextureQueryCall(spv::OpImageQueryLod, texture_parameters, false),
        type_float, 1);
  }
  if_signed.MakeBeginElse();
  spv::Id lod_unsigned;
  {
    texture_parameters.sampler = builder_->createBinOp(
        spv::OpSampledImage, builder_->makeSampledImageType(builder_->getTypeId(image_unsigned)),
        image_unsigned, sampler);
    lod_unsigned = builder_->createCompositeExtract(
        builder_->createTextureQueryCall(spv::OpImageQueryLod, texture_parameters, false),
        type_float, 1);
  }
  if_signed.MakeEndIf();
  return if_signed.CreateMergePhi(lod_signed, lod_unsigned);
}

}  // namespace rex::gpu
