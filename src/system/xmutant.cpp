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
#include <rex/system/xmutant.h>
#include <rex/system/xthread.h>

namespace rex::system {

MutantObject::MutantObject(bool initial_owner) {
  mutant = rex::platform::Mutant::Create(initial_owner);
  assert_not_null(mutant);
}

X_STATUS MutantRelease(MutantObject& mutant, bool abandon) {
  u32 expected = ThreadRecord::GetCurrentThread()->thread_id();
  mutant.owner_thread_id.compare_exchange_strong(expected, 0);
  assert_false(abandon);
  return mutant.mutant->Release() ? X_STATUS_SUCCESS : X_STATUS_MUTANT_NOT_OWNED;
}

void MutantAcquired(MutantObject& mutant) {
  mutant.owner_thread_id = ThreadRecord::GetCurrentThread()->thread_id();
}

}  // namespace rex::system
