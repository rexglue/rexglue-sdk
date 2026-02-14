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

#include <rex/system/export_resolver.h>
#include <rex/system/xmodule.h>

namespace rex {
class Runtime;

namespace system {

class KernelState;

class KernelModule : public XModule {
 public:
  KernelModule(KernelState* kernel_state, const std::string_view path);
  ~KernelModule() override;

  const std::string& path() const override { return path_; }
  const std::string& name() const override { return name_; }

  uint32_t GetProcAddressByOrdinal(uint16_t ordinal) override;
  uint32_t GetProcAddressByName(const std::string_view name) override;

 protected:
  Runtime* emulator_;
  memory::Memory* memory_;
  rex::runtime::ExportResolver* export_resolver_;

  std::string name_;
  std::string path_;

  rex::thread::global_critical_region global_critical_region_;
};

}  // namespace system
}  // namespace rex
