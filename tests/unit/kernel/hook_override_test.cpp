/**
 * @file        tests/unit/kernel/hook_override_test.cpp
 * @brief       A hook the executable defines replaces an export of the kernel or of XAM
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <atomic>

#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include "kernel/kernel_fixture.h"

namespace {

constexpr u32 kHookResult = 0x600D;
constexpr u32 kUntouched = 0xFFFF;

std::atomic<u32> g_xam_hook_calls{0};
std::atomic<u32> g_kernel_hook_calls{0};

}  // namespace

extern "C" {

REX_FUNC(__imp__XamUserGetSigninState);
REX_FUNC(__imp__XamGetSystemVersion);
REX_FUNC(__imp__KeGetCurrentProcessType);
REX_FUNC(__imp__RtlNtStatusToDosError);

REX_FUNC(__imp__XamUserGetSigninState) {
  g_xam_hook_calls.fetch_add(1);
  ctx.r3.u64 = kHookResult;
}

REX_FUNC(__imp__KeGetCurrentProcessType) {
  g_kernel_hook_calls.fetch_add(1);
  ctx.r3.u64 = kHookResult;
}

}  // extern "C"

using rex::test::Caller;
using rex::test::KernelFixture;

REX_TEST_CASE("a hook the executable defines replaces an XAM export", "[kernel][xam][hook]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_xam_hook_calls = 0;
    c.GetContext().r3.u64 = 0;
    __imp__XamUserGetSigninState(c.GetContext(), c.GetBase());
    CHECK(g_xam_hook_calls == 1);
    CHECK(c.GetContext().r3.u32 == kHookResult);
  });
}

REX_TEST_CASE("an XAM export the executable does not define runs the rexxam body",
              "[kernel][xam][hook]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    c.GetContext().r3.u64 = kUntouched;
    __imp__XamGetSystemVersion(c.GetContext(), c.GetBase());
    CHECK(c.GetContext().r3.u32 == 0);
  });
}

REX_TEST_CASE("a hook the executable defines replaces a kernel export", "[kernel][hook]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    g_kernel_hook_calls = 0;
    c.GetContext().r3.u64 = 0;
    __imp__KeGetCurrentProcessType(c.GetContext(), c.GetBase());
    CHECK(g_kernel_hook_calls == 1);
    CHECK(c.GetContext().r3.u32 == kHookResult);
  });
}

REX_TEST_CASE("a kernel export the executable does not define runs the kernel body",
              "[kernel][hook]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    c.GetContext().r3.u64 = 0xC0000022;
    __imp__RtlNtStatusToDosError(c.GetContext(), c.GetBase());
    CHECK(c.GetContext().r3.u32 == 5);
  });
}
