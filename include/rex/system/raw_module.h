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

#include <string>

#include <rex/system/module.h>

namespace rex::runtime {

class RawModule : public Module {
 public:
  explicit RawModule(Processor* processor);
  ~RawModule() override;

  bool LoadFile(uint32_t base_address, const std::filesystem::path& path);

  // Set address range if you've already allocated memory and placed code
  // in it.
  void SetAddressRange(uint32_t base_address, uint32_t size);

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return is_executable_; }
  void set_name(const std::string_view name) { name_ = name; }
  void set_executable(bool is_executable) { is_executable_ = is_executable; }

  bool ContainsAddress(uint32_t address) override;

  // Binary introspection overrides
  uint32_t base_address() const override { return base_address_; }
  uint32_t image_size() const override { return high_address_ - low_address_; }
  uint32_t entry_point() const override { return low_address_; }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;

 private:
  std::string name_;
  bool is_executable_ = false;
  uint32_t base_address_;
  uint32_t low_address_;
  uint32_t high_address_;
};

}  // namespace rex::runtime
// (removed orphan xe namespace)
