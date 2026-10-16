/**
 * @file        gpu/xenos/shared_cvars.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * Returns the xenos.dump_shaders path.
 *
 * @return The path, empty when shader dumping is off.
 */
REX_GPU_XENOS_API std::string GetDumpShaders();

/**
 * Reports xenos.allow_invalid_fetch_constants.
 *
 * @return True when invalid fetch constants are tolerated.
 */
REX_GPU_XENOS_API bool GetAllowInvalidFetchConstants();

/**
 * Returns xenos.query_occlusion_fake_sample_count.
 *
 * @return The fake sample count an occlusion query reports.
 */
REX_GPU_XENOS_API i32 GetQueryOcclusionFakeSampleCount();

}  // namespace rex::gpu
