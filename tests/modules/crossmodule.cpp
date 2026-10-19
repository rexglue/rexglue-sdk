/**
 * @file        tests/modules/crossmodule.cpp
 * @brief       Recompiled library that exports a function or imports it from another module
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/types.h>
#include <rex/image_info.h>
#include <rex/export.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/runtime/module_import.h>

namespace {

constexpr rex::u32 kImageBase = REX_TEST_IMAGE_BASE;
constexpr rex::u32 kImageSize = 0x10000;
constexpr rex::u32 kFunctionAddress = kImageBase + 0x100;

#ifdef REX_TEST_EXPORTER
void Function(PPCContext& ctx, rex::u8*) {
  ctx.r3.u64 = 0xA1;
}
#else
void KernelThunk(PPCContext& ctx, rex::u8*) {
  ctx.r3.u64 = 0xB2;
}

void IndirectKernelFunction(PPCContext& ctx, rex::u8* base) {
  const rex::u32 slot = kImageBase + 0x2020;
  const rex::u32 target = *reinterpret_cast<rex::be_u32*>(base + slot);
  rex::ppc::ResolveFunction(target)(ctx, base);
}

void DirectFunction(PPCContext& ctx, rex::u8* base) {
  static rex::runtime::ModuleImport target{"exporter", 1};
  rex::runtime::ResolveModuleImport(target)(ctx, base);
}

void IndirectFunction(PPCContext& ctx, rex::u8* base) {
  const rex::u32 slot = kImageBase + 0x2000;
  const rex::u32 target = *reinterpret_cast<rex::be_u32*>(base + slot);
  rex::ppc::ResolveFunction(target)(ctx, base);
}
#endif

const rex::PPCImage kImageInfo = {
    .code_base = kImageBase,
    .code_size = kImageSize,
    .image_base = kImageBase,
    .image_size = kImageSize,
    .func_mappings = nullptr,
};

}  // namespace

extern "C" REX_DETAIL_EXPORT void ReXRegisterModule(rex::runtime::IModuleRegistrar* registrar) {
#ifdef REX_TEST_EXPORTER
  registrar->SetFunction(kFunctionAddress, Function);
#else
  registrar->SetFunction(kFunctionAddress, DirectFunction);
  registrar->SetFunction(kFunctionAddress + 0x100, IndirectFunction);
  registrar->SetFunction(kFunctionAddress + 0x200, IndirectKernelFunction);
  registrar->SetFunction(kImageBase + 0x2028, KernelThunk);
#endif
}

extern "C" REX_DETAIL_EXPORT const rex::PPCImage* ReXGetModuleImage() {
  return &kImageInfo;
}
