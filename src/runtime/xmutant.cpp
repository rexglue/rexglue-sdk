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

#include <rex/logging.h>
#include <rex/runtime/xmutant.h>
#include <rex/runtime/xthread.h>

namespace rex::runtime {

namespace {

u32 CurrentThreadId() {
  return ThreadRecord::IsInThread() ? ThreadRecord::GetCurrentThread()->thread_id() : 0;
}

}  // namespace

MutantObject::MutantObject(bool initial_owner) {
  mutant = rex::platform::Mutant::Create(initial_owner);
  assert_not_null(mutant);
  if (initial_owner) {
    owner_thread_id = CurrentThreadId();
    hold_count = 1;
  }
}

X_STATUS MutantRelease(MutantObject& mutant, bool abandon, i32* previous_state) {
  assert_false(abandon);
  const u32 self = CurrentThreadId();
  if (!self || mutant.owner_thread_id.load() != self) {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  const i32 held = mutant.hold_count.fetch_sub(1);
  if (held == 1) {
    mutant.owner_thread_id = 0;
  }
  if (!mutant.mutant->Release()) {
    mutant.owner_thread_id = self;
    mutant.hold_count.fetch_add(1);
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  if (previous_state) {
    *previous_state = 1 - held;
  }
  return X_STATUS_SUCCESS;
}

void MutantAcquired(MutantObject& mutant) {
  const u32 self = ThreadRecord::GetCurrentThread()->thread_id();
  if (mutant.owner_thread_id.exchange(self) == self) {
    mutant.hold_count.fetch_add(1);
  } else {
    mutant.hold_count = 1;
  }
}

}  // namespace rex::runtime
