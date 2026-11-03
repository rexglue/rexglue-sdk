/**
 * @file        tests/modules/recompiled.cpp
 * @brief       Recompiled library with one function, built once per image the tests load
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/types.h>
#include <rex/ppc/image.h>
#include <rex/platform.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/kernel/function_dispatcher.h>

namespace {

constexpr rex::u32 kImageBase = REX_TEST_IMAGE_BASE;
constexpr rex::u32 kImageSize = 0x10000;
constexpr rex::u32 kFunctionAddress = kImageBase + 0x100;

void Function(PPCContext& ctx, rex::u8*) {
  ctx.r3.u64 = REX_TEST_RESULT;
}

const rex::ppc::PPCImage kImageInfo = {
    .code_base = kImageBase,
    .code_size = kImageSize,
    .image_base = kImageBase,
    .image_size = kImageSize,
    .func_mappings = nullptr,
};

}  // namespace

extern "C" REX_DETAIL_EXPORT void ReXRegisterModule(rex::kernel::IFunctionRegistrar* registrar) {
  registrar->SetFunction(kFunctionAddress, Function);
}

extern "C" REX_DETAIL_EXPORT const rex::ppc::PPCImage* ReXGetModuleImage() {
  return &kImageInfo;
}
