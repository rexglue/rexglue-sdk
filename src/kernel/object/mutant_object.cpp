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

#include <rex/logging/log.h>
#include <rex/kernel/heap.h>
#include <rex/kernel/object/mutant_object.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/platform/sync.h>

namespace rex::kernel {

namespace {

u32 CurrentThreadID() {
  return KernelThread::IsInThread() ? KernelThread::GetCallingThread()->GetThreadID() : 0;
}

}  // namespace

MutantObject::MutantObject(bool initial_owner, u32 guest_mutant_in)
    : guest_mutant(guest_mutant_in) {
  mutant = rex::platform::Mutant::Create(initial_owner);
  REX_ASSERT(mutant != nullptr);
  if (initial_owner) {
    owner_thread_id = CurrentThreadID();
    hold_count = 1;
  }
}

X_STATUS MutantObject::Release(bool abandon, i32* previous_state) {
  const u32 self = CurrentThreadID();
  if (!self || owner_thread_id.load() != self) {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  if (abandon) {
    const i32 held = hold_count.exchange(0);
    owner_thread_id = 0;
    abandoned = true;
    for (i32 i = 0; i < held; ++i) {
      mutant->Release();
    }
    if (previous_state) {
      *previous_state = 1 - held;
    }
    return X_STATUS_SUCCESS;
  }
  const i32 held = hold_count.fetch_sub(1);
  if (held == 1) {
    owner_thread_id = 0;
  }
  if (!mutant->Release()) {
    owner_thread_id = self;
    hold_count.fetch_add(1);
    return X_STATUS_MUTANT_NOT_OWNED;
  }
  if (previous_state) {
    *previous_state = 1 - held;
  }
  return X_STATUS_SUCCESS;
}

bool MutantObject::OnAcquired() {
  KernelThread* thread = KernelThread::GetCallingThread();
  const u32 self = thread->GetThreadID();
  if (owner_thread_id.exchange(self) == self) {
    hold_count.fetch_add(1);
  } else {
    hold_count = 1;
  }
  if (guest_mutant) {
    thread->GetMemory()->TranslateVirtual<X_KMUTANT*>(guest_mutant)->abandoned = 0;
  }
  return abandoned.exchange(false);
}

}  // namespace rex::kernel
