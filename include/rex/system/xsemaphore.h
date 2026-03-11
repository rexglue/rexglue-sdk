#pragma once
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

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

namespace rex::system {

struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header;
  rex::be<uint32_t> limit;
};
static_assert_size(X_KSEMAPHORE, 0x14);

class XSemaphore : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Semaphore;

  explicit XSemaphore(KernelState* kernel_state);
  ~XSemaphore() override;

  [[nodiscard]] bool Initialize(int32_t initial_count, int32_t maximum_count);
  [[nodiscard]] bool InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  [[nodiscard]] bool ReleaseSemaphore(int32_t release_count, int32_t* out_previous_count);

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XSemaphore> Restore(KernelState* kernel_state, stream::ByteStream* stream);

 protected:
  rex::thread::WaitHandle* GetWaitHandle() override { return semaphore_.get(); }

 private:
  std::unique_ptr<rex::thread::Semaphore> semaphore_;
  uint32_t maximum_count_ = 0;
};

}  // namespace rex::system
