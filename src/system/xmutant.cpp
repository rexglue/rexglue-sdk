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
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmutant.h>
#include <rex/system/xthread.h>

namespace rex::system {

XMutant::XMutant(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() = default;

void XMutant::Initialize(bool initial_owner) {
  assert_false(mutant_);

  mutant_ = rex::thread::Mutant::Create(initial_owner);
  assert_not_null(mutant_);
}

void XMutant::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(mutant_);

  // Haven't seen this yet, but it's possible.
  assert_always();
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait) {
  // Call should succeed if we own the mutant, so go ahead and do this.
  if (owning_thread_ == XThread::GetCurrentThread()) {
    owning_thread_ = nullptr;
  }

  // TODO(benvanik): abandoning.
  assert_false(abandon);
  if (mutant_->Release()) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
}

bool XMutant::Save(stream::ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  uint32_t owning_thread_handle = owning_thread_ ? owning_thread_->handle() : 0;
  stream->Write<uint32_t>(owning_thread_handle);
  REXSYS_DEBUG("XMutant {:08X} (owner: {:08X})", handle(), owning_thread_handle);

  return true;
}

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state, stream::ByteStream* stream) {
  auto mutant = new XMutant();
  mutant->kernel_state_ = kernel_state;

  if (!mutant->RestoreObject(stream)) {
    delete mutant;
    return nullptr;
  }

  mutant->Initialize(false);

  auto owning_thread_handle = stream->Read<uint32_t>();
  if (owning_thread_handle) {
    mutant->owning_thread_ =
        kernel_state->object_table()->LookupObject<XThread>(owning_thread_handle).get();
    mutant->owning_thread_->AcquireMutantOnStartup(retain_object(mutant));
  }

  return object_ref<XMutant>(mutant);
}

void XMutant::WaitCallback() {
  owning_thread_ = XThread::GetCurrentThread();
}

}  // namespace rex::system
