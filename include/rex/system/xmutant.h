#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state);
  ~XMutant() override;

  void Initialize(bool initial_owner);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait);

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XMutant> Restore(KernelState* kernel_state, stream::ByteStream* stream);

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return mutant_.get(); }
  void WaitCallback() override;

 private:
  XMutant();

  std::unique_ptr<rex::thread::Mutant> mutant_;
  XThread* owning_thread_ = nullptr;
};

}  // namespace rex::system
