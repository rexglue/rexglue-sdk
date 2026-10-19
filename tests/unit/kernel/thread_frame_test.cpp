/**
 * @file        tests/unit/kernel/thread_frame_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>

#include <rex/ppc/function.h>

#include "kernel_fixture.h"

using rex::u32;

using rex::test::Caller;
using rex::test::Join;
using rex::test::KernelFixture;

namespace {

struct EntryFrame {
  u32 stack_base = 0;
  u32 r1 = 0;
  u32 back_chain = 0;
  u32 root_nonzero_words = 0;
};

}  // namespace

REX_TEST_CASE("A thread entry runs in a call frame under a zeroed root frame",
              "[kernel][threads][frame]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto frame = std::make_shared<EntryFrame>();
    const u32 handle = c.Spawn([frame](Caller& t) {
      const u32 kthread = t.Load32(t.GetContext().r13.u32 + 0x100);
      frame->stack_base = t.Load32(kthread + 0x5C);
      frame->r1 = t.GetContext().r1.u32;
      frame->back_chain = t.Load32(frame->r1);
      const u32 root = frame->stack_base - rex::ppc::kLinkageSize;
      for (u32 offset = 0; offset < rex::ppc::kLinkageSize; offset += 4) {
        frame->root_nonzero_words += t.Load32(root + offset) != 0;
      }
    });
    Join(c, handle);
    const u32 root = frame->stack_base - rex::ppc::kLinkageSize;
    CHECK(frame->back_chain == root);
    CHECK(frame->r1 == root - rex::ppc::kLinkageSize);
    CHECK(frame->root_nonzero_words == 0);
  });
}

REX_TEST_CASE("A write over the root frame leaves the TLS block alone",
              "[kernel][threads][frame]") {
  KernelFixture fixture;
  fixture.RunOnGuestThread([](Caller& c) {
    auto words = std::make_shared<u32>();
    const u32 handle = c.Spawn([words](Caller& t) {
      const u32 kthread = t.Load32(t.GetContext().r13.u32 + 0x100);
      const u32 stack_base = t.Load32(kthread + 0x5C);
      const u32 root = stack_base - rex::ppc::kLinkageSize;
      t.Store32(stack_base, 0x12345678);
      for (u32 offset = 0; offset < rex::ppc::kLinkageSize; offset += 4) {
        t.Store32(root + offset, 0xFFFFFFFF);
      }
      *words = t.Load32(stack_base);
    });
    Join(c, handle);
    CHECK(*words == 0x12345678);
  });
}
