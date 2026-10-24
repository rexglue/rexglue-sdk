/**
 * @file        tests/unit/gpu/primitive_processor_test.cpp
 * @brief       Sizes the primitive processor derives for its converted index buffers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstdint>

#include <rex/types.h>

#include "gpu/xenos/index_conversion.h"
#include "gpu/xenos/primitive_processor.h"

namespace {

struct SizeAccess : rex::gpu::PrimitiveProcessor {
  static constexpr rex::u32 kMinRequiredSize = kMinRequiredConvertedIndexBufferSize;
};

}  // namespace

REX_TEST_CASE("The minimum converted index buffer adds the SIMD padding to the triangle fan size",
              "[gpu][primitive_processor]") {
  constexpr rex::u32 kTriangleFanListBytes = sizeof(rex::u32) * (UINT16_MAX - 2) * 3;
  CHECK(SizeAccess::kMinRequiredSize ==
        kTriangleFanListBytes + REXGPU_PRIMITIVE_PROCESSOR_SIMD_SIZE);
}
