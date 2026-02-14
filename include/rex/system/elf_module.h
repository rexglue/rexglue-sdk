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
#include <vector>

#include <rex/system/module.h>

namespace rex::system {
class KernelState;
}  // namespace rex::system

namespace rex::runtime {

// ELF module: Used to load libxenon executables.
class ElfModule : public Module {
 public:
  ElfModule(Processor* processor, rex::system::KernelState* kernel_state);
  virtual ~ElfModule();

  bool loaded() const { return loaded_; }
  const std::string& name() const override { return name_; }
  bool is_executable() const override;
  const std::string& path() const { return path_; }

  // Binary introspection overrides
  uint32_t base_address() const override { return base_address_; }
  uint32_t image_size() const override { return image_size_; }
  uint32_t entry_point() const override { return entry_point_; }

  bool Load(const std::string_view name, const std::string_view path, const void* elf_addr,
            size_t elf_length);
  bool Unload();

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;

 private:
  std::string name_;
  std::string path_;
  rex::system::KernelState* kernel_state_;

  bool loaded_ = false;
  std::vector<uint8_t> elf_header_mem_;  // Holds the ELF header
  uint32_t entry_point_ = 0;
  uint32_t base_address_ = 0;
  uint32_t image_size_ = 0;
};

}  // namespace rex::runtime
