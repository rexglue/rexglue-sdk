#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <memory>
#include <unordered_map>

#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::system {

class XSymbolicLink : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::SymbolicLink;

  explicit XSymbolicLink(KernelState* kernel_state);
  ~XSymbolicLink() override;

  void Initialize(const std::string_view path, const std::string_view target);

  bool Save(stream::ByteStream* stream) override;
  static object_ref<XSymbolicLink> Restore(KernelState* kernel_state, stream::ByteStream* stream);

  const std::string& path() const { return path_; }
  const std::string& target() const { return target_; }

 private:
  XSymbolicLink();

  std::string path_;
  std::string target_;
};

}  // namespace rex::system
