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

#pragma once

#include <string>
#include <vector>

#include <rex/macros.h>
#include <rex/runtime/module.h>

namespace rex::runtime {

/** Loads libxenon executables. */
class REX_RUNTIME_API ElfModule : public Module {
 public:
  /**
   * Builds an unloaded module.
   *
   * @param function_dispatcher The dispatcher whose memory the module loads into.
   */
  explicit ElfModule(FunctionDispatcher* function_dispatcher);
  /** Destroys the module. */
  virtual ~ElfModule();

  bool loaded() const { return loaded_; }
  const std::string& name() const override { return name_; }
  /**
   * Reports whether the image has an entry point.
   *
   * @return True when it does.
   */
  bool is_executable() const override;
  const std::string& path() const { return path_; }

  uint32_t base_address() const override { return base_address_; }
  uint32_t image_size() const override { return image_size_; }
  uint32_t entry_point() const override { return entry_point_; }

  /**
   * Copies the loadable segments of an ELF image into guest memory.
   *
   * @param name The module name.
   * @param path The path the image came from.
   * @param elf_addr The host address of the ELF image.
   * @param elf_length The size of the image in bytes.
   * @return True on success, false when the data is not an ELF or has no entry point.
   */
  bool Load(const std::string_view name, const std::string_view path, const void* elf_addr,
            size_t elf_length);
  /**
   * Marks the module unloaded.
   *
   * @return True.
   */
  bool Unload();

 private:
  std::string name_;
  std::string path_;

  bool loaded_ = false;
  std::vector<uint8_t> elf_header_mem_;
  uint32_t entry_point_ = 0;
  uint32_t base_address_ = 0;
  uint32_t image_size_ = 0;
};

}  // namespace rex::runtime
