/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_BINDING_LAYOUT_H_
#define XENIA_GPU_METAL_METAL_BINDING_LAYOUT_H_

#include "rex/graphics/texture_sign.h"
#include "gpu/xenos/pipeline/shader/shader.h"
#include <bit>  // std::popcount replaced rex::bit_count upstream
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rex/math.h"
#include <rex/xenos/register.h>
#include "rex/graphics/shader.h"
#include "rex/graphics/texture_util.h"

namespace rex {
namespace graphics {
namespace metal {

enum class MetalBindingKind : uint8_t {
  kSrvTable,
  kUavTable,
  kSamplerTable,
  kConstantBuffer,
  kFunctionConstantTable,
};

enum class MetalBindingVisibility : uint8_t {
  kAll,
  kVertex,
  kHull,
  kDomain,
  kPixel,
};

enum class MetalBindingSlot : uint32_t {
  kSrvSpace0,
  kSrvSpace1,
  kSrvSpace2,
  kSrvSpace3,
  kSrvSpace10,
  kUavSpace0,
  kUavSpace1,
  kUavSpace2,
  kUavSpace3,
  kSamplerSpace0,
  kCbvSystem,
  kCbvFloatVertex,
  kCbvBoolLoop,
  kCbvFetchVertex,
  kCbvDescriptorIndicesVertex,
  kCbvFloatHull,
  kCbvFetchHull,
  kCbvDescriptorIndicesHull,
  kCbvFloatDomain,
  kCbvFetchDomain,
  kCbvDescriptorIndicesDomain,
  kCbvFloatPixel,
  kCbvFetchPixel,
  kCbvDescriptorIndicesPixel,
  kFunctionConstants,
  kCount,
};

constexpr uint32_t kMetalFunctionConstantRegisterSpace = 2147420894u;
constexpr uint32_t kMetalBindingSlotCount = uint32_t(MetalBindingSlot::kCount);
constexpr uint32_t kMetalBindfulResourceDescriptorCount = 1025;
constexpr uint32_t kMetalBindfulSamplerDescriptorCount = 257;

enum class MetalBindingDescriptorCountPolicy : uint8_t {
  kNone,
  kResourceHeap,
  kSamplerHeap,
  kSingle,
};

struct MetalBindingParameter {
  MetalBindingSlot slot;
  MetalBindingKind kind;
  uint32_t shader_register;
  uint32_t register_space;
  MetalBindingVisibility visibility;
  MetalBindingDescriptorCountPolicy descriptor_count_policy;
};

constexpr std::array<MetalBindingParameter, kMetalBindingSlotCount>
    kMetalBindingLayout = {{
        {MetalBindingSlot::kSrvSpace0, MetalBindingKind::kSrvTable, 0, 0,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kSrvSpace1, MetalBindingKind::kSrvTable, 0, 1,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kSrvSpace2, MetalBindingKind::kSrvTable, 0, 2,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kSrvSpace3, MetalBindingKind::kSrvTable, 0, 3,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kSrvSpace10, MetalBindingKind::kSrvTable, 0, 10,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kUavSpace0, MetalBindingKind::kUavTable, 0, 0,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kUavSpace1, MetalBindingKind::kUavTable, 0, 1,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kUavSpace2, MetalBindingKind::kUavTable, 0, 2,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kUavSpace3, MetalBindingKind::kUavTable, 0, 3,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kResourceHeap},
        {MetalBindingSlot::kSamplerSpace0, MetalBindingKind::kSamplerTable, 0,
         0, MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kSamplerHeap},
        {MetalBindingSlot::kCbvSystem, MetalBindingKind::kConstantBuffer, 0, 0,
         MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFloatVertex, MetalBindingKind::kConstantBuffer,
         1, 0, MetalBindingVisibility::kVertex,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvBoolLoop, MetalBindingKind::kConstantBuffer, 2,
         0, MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFetchVertex, MetalBindingKind::kConstantBuffer,
         3, 0, MetalBindingVisibility::kVertex,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvDescriptorIndicesVertex,
         MetalBindingKind::kConstantBuffer, 4, 0,
         MetalBindingVisibility::kVertex,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFloatHull, MetalBindingKind::kConstantBuffer, 1,
         0, MetalBindingVisibility::kHull,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFetchHull, MetalBindingKind::kConstantBuffer, 3,
         0, MetalBindingVisibility::kHull,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvDescriptorIndicesHull,
         MetalBindingKind::kConstantBuffer, 4, 0, MetalBindingVisibility::kHull,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFloatDomain, MetalBindingKind::kConstantBuffer,
         1, 0, MetalBindingVisibility::kDomain,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFetchDomain, MetalBindingKind::kConstantBuffer,
         3, 0, MetalBindingVisibility::kDomain,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvDescriptorIndicesDomain,
         MetalBindingKind::kConstantBuffer, 4, 0,
         MetalBindingVisibility::kDomain,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFloatPixel, MetalBindingKind::kConstantBuffer, 1,
         0, MetalBindingVisibility::kPixel,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvFetchPixel, MetalBindingKind::kConstantBuffer, 3,
         0, MetalBindingVisibility::kPixel,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kCbvDescriptorIndicesPixel,
         MetalBindingKind::kConstantBuffer, 4, 0,
         MetalBindingVisibility::kPixel,
         MetalBindingDescriptorCountPolicy::kNone},
        {MetalBindingSlot::kFunctionConstants,
         MetalBindingKind::kFunctionConstantTable, 0,
         kMetalFunctionConstantRegisterSpace, MetalBindingVisibility::kAll,
         MetalBindingDescriptorCountPolicy::kSingle},
    }};

static_assert(uint32_t(MetalBindingSlot::kSrvSpace0) == 0);
static_assert(uint32_t(MetalBindingSlot::kUavSpace0) == 5);
static_assert(uint32_t(MetalBindingSlot::kCbvSystem) == 10);
static_assert(uint32_t(MetalBindingSlot::kCbvFloatPixel) == 21);
static_assert(uint32_t(MetalBindingSlot::kFunctionConstants) == 24);
static_assert(kMetalBindingSlotCount == 25);

constexpr bool IsMetalBindingLayoutValid() {
  for (uint32_t i = 0; i < kMetalBindingSlotCount; ++i) {
    const MetalBindingParameter& parameter = kMetalBindingLayout[i];
    if (uint32_t(parameter.slot) != i) {
      return false;
    }
    switch (parameter.kind) {
      case MetalBindingKind::kSrvTable:
      case MetalBindingKind::kUavTable:
        if (parameter.descriptor_count_policy !=
            MetalBindingDescriptorCountPolicy::kResourceHeap) {
          return false;
        }
        break;
      case MetalBindingKind::kSamplerTable:
        if (parameter.descriptor_count_policy !=
            MetalBindingDescriptorCountPolicy::kSamplerHeap) {
          return false;
        }
        break;
      case MetalBindingKind::kFunctionConstantTable:
        if (parameter.descriptor_count_policy !=
            MetalBindingDescriptorCountPolicy::kSingle) {
          return false;
        }
        break;
      case MetalBindingKind::kConstantBuffer:
        if (parameter.descriptor_count_policy !=
            MetalBindingDescriptorCountPolicy::kNone) {
          return false;
        }
        break;
    }
  }
  return true;
}
static_assert(IsMetalBindingLayoutValid());

constexpr uint32_t GetMetalBindingDescriptorCount(
    const MetalBindingParameter& parameter, bool bindless_resources_used) {
  switch (parameter.descriptor_count_policy) {
    case MetalBindingDescriptorCountPolicy::kResourceHeap:
      return bindless_resources_used ? UINT32_MAX
                                     : kMetalBindfulResourceDescriptorCount;
    case MetalBindingDescriptorCountPolicy::kSamplerHeap:
      return bindless_resources_used ? UINT32_MAX
                                     : kMetalBindfulSamplerDescriptorCount;
    case MetalBindingDescriptorCountPolicy::kSingle:
      return 1;
    case MetalBindingDescriptorCountPolicy::kNone:
    default:
      return 0;
  }
}

constexpr uint32_t MetalBindingSlotIndex(MetalBindingSlot slot) {
  return uint32_t(slot);
}

constexpr uint32_t kMetalBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
constexpr uint32_t kMetalFetchConstantsSize = 32 * 6 * sizeof(uint32_t);

inline bool AreMetalConstantBankBasesSupported(uint32_t vertex_base,
                                               uint32_t pixel_base) {
  if (vertex_base != 0 && vertex_base != 0x000FF000u) {
    return false;
  }
  return pixel_base == 0 || pixel_base == 0x000FF100u;
}

inline uint32_t GetMetalPackedFloatConstantCount(
    const ::rex::gpu::Shader::ConstantRegisterMap& map) {
  uint32_t count = 0;
  for (uint64_t bitmap : map.float_bitmap) {
    count += std::popcount(bitmap);
  }
  return count;
}

inline bool PackMetalFloatConstants(const RegisterFile& regs,
                                    const ::rex::gpu::Shader::ConstantRegisterMap& map,
                                    uint32_t register_base, void* destination,
                                    size_t destination_size) {
  const uint32_t count = GetMetalPackedFloatConstantCount(map);
  const size_t required_size = sizeof(float) * 4 * std::max(count, 1u);
  if (!destination || destination_size < required_size) {
    return false;
  }
  uint8_t* destination_bytes = static_cast<uint8_t*>(destination);
  if (!count) {
    std::memset(destination_bytes, 0, required_size);
    return true;
  }
  for (uint32_t bitmap_index = 0; bitmap_index < 4; ++bitmap_index) {
    uint64_t bitmap = map.float_bitmap[bitmap_index];
    uint32_t constant_index;
    while (rex::bit_scan_forward(bitmap, &constant_index)) {
      bitmap &= bitmap - 1;  // clear the lowest set bit
      std::memcpy(
          destination_bytes,
          &regs[register_base + (bitmap_index << 8) + (constant_index << 2)],
          4 * sizeof(float));
      destination_bytes += 4 * sizeof(float);
    }
  }
  return true;
}

// Translator limits: up to 255 textures and 127 samplers share one flat b4
// index space per shader stage.
constexpr uint32_t kMetalMaxDescriptorIndexCount = 255 + 127;

struct MetalDescriptorIndexPlan {
  uint32_t count = 1;
  bool valid = true;
};

template <typename TextureBindings, typename SamplerBindings>
inline MetalDescriptorIndexPlan GetMetalDescriptorIndexPlan(
    const TextureBindings& textures, const SamplerBindings& samplers) {
  MetalDescriptorIndexPlan plan;
  std::array<bool, kMetalMaxDescriptorIndexCount> assigned = {};
  auto add_binding = [&](const auto& binding) {
    const uint32_t index = binding.bindless_descriptor_index;
    if (index >= assigned.size() || assigned[index]) {
      plan.valid = false;
      return;
    }
    assigned[index] = true;
    plan.count = std::max(plan.count, index + 1);
  };
  for (const auto& binding : textures) {
    add_binding(binding);
  }
  for (const auto& binding : samplers) {
    add_binding(binding);
  }
  return plan;
}

inline bool InitializeMetalDescriptorIndices(
    const MetalDescriptorIndexPlan& plan, void* destination,
    size_t destination_size) {
  const size_t required_size = size_t(plan.count) * sizeof(uint32_t);
  if (!plan.valid || !destination || !plan.count ||
      plan.count > kMetalMaxDescriptorIndexCount ||
      destination_size < required_size) {
    return false;
  }
  std::memset(destination, 0, required_size);
  return true;
}

// Whether the translated shader can select this sign interpretation. The
// shared predicates match the D3D12 and Vulkan binding gates. They inspect all
// four packed signs, so the result is deliberately a conservative superset of
// the translator's used-component reduction: it may retain an unused
// interpretation, but it never rejects one the shader can select.
constexpr bool IsMetalTextureSignInterpretationReachable(uint8_t swizzled_signs,
                                                         bool is_signed) {
  return is_signed ? texture_util::IsAnySignSigned(swizzled_signs)
                   : texture_util::IsAnySignNotSigned(swizzled_signs);
}

enum MetalBindingStageMask : uint32_t {
  kMetalBindingStageVertex = 1u << 0,
  kMetalBindingStageObject = 1u << 1,
  kMetalBindingStageMesh = 1u << 2,
  kMetalBindingStageFragment = 1u << 3,
};

// The per-draw residency stage mask: exactly the stages the draw's shaders run
// on -- vertex+fragment for a plain draw, object+mesh+fragment under
// geometry/tessellation emulation, since no vertex stage runs under
// mesh-shader emulation.
//
// A wider, unconditional all-graphics-stages mask (XeniOS's shape) was tried as
// a page-fault candidate under port-xenios-tessellation 5.3 and removed at
// closeout: its A/B ran at an exposure now known to be underpowered, so it is
// recorded as UNSUPPORTED rather than falsified. If revisited, the relevant
// properties are that widening can only ADD declarations (so it cannot itself
// cause a residency fault) and that a wrongly-NARROW mask is a page fault --
// which is what makes this narrow mask an optimization depending on the stage
// model being exactly right.
constexpr uint32_t GetMetalBindingStageMask(bool geometry_emulation) {
  return geometry_emulation
             ? (kMetalBindingStageObject | kMetalBindingStageMesh |
                kMetalBindingStageFragment)
             : (kMetalBindingStageVertex | kMetalBindingStageFragment);
}

struct MetalBindingAddresses {
  uint64_t null_buffer = 0;
  uint64_t view_heap = 0;
  uint64_t sampler_heap = 0;
  uint64_t shared_memory_srv = 0;
  uint64_t null_uav = 0;
  uint64_t shared_memory_uav = 0;
  uint64_t system_constants = 0;
  uint64_t float_vertex = 0;
  uint64_t bool_loop = 0;
  uint64_t fetch = 0;
  uint64_t descriptor_indices_vertex = 0;
  uint64_t float_pixel = 0;
  uint64_t descriptor_indices_pixel = 0;
};

// The hull/domain CBV slots (kCbvFloatHull .. kCbvDescriptorIndicesDomain)
// stay in the root layout at the null buffer: the MSC object/mesh
// tessellation route that aliased them was deleted on the host route's
// promotion (implement-metal-host-tessellation, 2026-09-20), and the slot set
// is part of the root signature every cached metallib was converted against.
inline std::array<uint64_t, kMetalBindingSlotCount>
BuildMetalTopLevelArgumentBuffer(const MetalBindingAddresses& addresses,
                                 bool pixel_shader_present,
                                 bool memexport_draw) {
  std::array<uint64_t, kMetalBindingSlotCount> result;
  result.fill(addresses.null_buffer);
  result[MetalBindingSlotIndex(MetalBindingSlot::kSrvSpace0)] =
      addresses.shared_memory_srv;
  result[MetalBindingSlotIndex(MetalBindingSlot::kSrvSpace1)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kSrvSpace2)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kSrvSpace3)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kSrvSpace10)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kUavSpace0)] =
      memexport_draw ? addresses.shared_memory_uav : addresses.null_uav;
  result[MetalBindingSlotIndex(MetalBindingSlot::kUavSpace1)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kUavSpace2)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kUavSpace3)] =
      addresses.view_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kSamplerSpace0)] =
      addresses.sampler_heap;
  result[MetalBindingSlotIndex(MetalBindingSlot::kCbvSystem)] =
      addresses.system_constants;
  result[MetalBindingSlotIndex(MetalBindingSlot::kCbvFloatVertex)] =
      addresses.float_vertex;
  result[MetalBindingSlotIndex(MetalBindingSlot::kCbvBoolLoop)] =
      addresses.bool_loop;
  result[MetalBindingSlotIndex(MetalBindingSlot::kCbvFetchVertex)] =
      addresses.fetch;
  result[MetalBindingSlotIndex(MetalBindingSlot::kCbvDescriptorIndicesVertex)] =
      addresses.descriptor_indices_vertex;
  if (pixel_shader_present) {
    result[MetalBindingSlotIndex(MetalBindingSlot::kCbvFloatPixel)] =
        addresses.float_pixel;
    result[MetalBindingSlotIndex(MetalBindingSlot::kCbvFetchPixel)] =
        addresses.fetch;
    result[MetalBindingSlotIndex(
        MetalBindingSlot::kCbvDescriptorIndicesPixel)] =
        addresses.descriptor_indices_pixel;
  }
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_BINDING_LAYOUT_H_
