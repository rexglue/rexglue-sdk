/**
 * @file        tests/unit/ppc/midasm_generated/generated_hooks.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>

#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/call.h>
#include <rex/ppc/context.h>
#include <rex/ppc/fpu.h>
#include <rex/ppc/stack.h>
#include <rex/ppc/vmx.h>
#include <rex/types.h>
#include <rex/ppc/midasm.h>

struct GeneratedWidget {
  u32 value;
};

#include "declarations.inc"

REX_MIDASM_HOOK(GeneratedPlain, (u32 size, PPCRegister& held)) {
  held.u64 = size;
}

REX_MIDASM_HOOK(GeneratedPointers,
                (GeneratedWidget * widget, rex::memory::MappedPtr<GeneratedWidget> mapped)) {
  widget->value = mapped->value;
}

REX_MIDASM_HOOK(GeneratedConstPointer, (const GeneratedWidget* widget)) {
  (void)widget->value;
}

void GeneratedCallSite(PPCContext& ctx, u8* base) {
#include "calls.inc"
}

namespace {

alignas(16) u8 g_guest_memory[0x10000];

}  // namespace

int main() {
  constexpr u32 kWidget = 0x100;
  constexpr u32 kSource = 0x200;
  auto* widget = reinterpret_cast<GeneratedWidget*>(g_guest_memory + kWidget);
  auto* source = reinterpret_cast<GeneratedWidget*>(g_guest_memory + kSource);
  widget->value = 1;
  source->value = 0x5A5A;
  PPCContext ctx{};
  ctx.r3.u64 = 7;
  ctx.r5.u64 = 0xFFFFFFFF00000000ull | kWidget;
  ctx.r27.u64 = kWidget;
  ctx.r28.u64 = kSource;
  GeneratedCallSite(ctx, g_guest_memory);
  if (widget->value != 0x5A5A || ctx.r4.u64 != 7) {
    std::printf("widget %x held %llu\n", widget->value,
                static_cast<unsigned long long>(ctx.r4.u64));
    return 1;
  }
  return 0;
}
