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

#include <rex/platform.h>
#include <rex/types.h>
#include <rex/export.h>
#include <rex/runtime/module.h>

namespace rex::runtime {

/** Loads libxenon executables. */
class REX_RUNTIME_API ELFModule : public Module {
 public:
  /**
   * Builds an unloaded module.
   *
   * @param function_dispatcher The dispatcher whose memory the module loads into.
   */
  explicit ELFModule(FunctionDispatcher* function_dispatcher);
  /** Destroys the module. */
  virtual ~ELFModule();

  /**
   * Reports whether the image is loaded.
   *
   * @return True when the image is loaded.
   */
  bool IsLoaded() const { return loaded_; }
  const std::string& GetName() const override { return name_; }
  /**
   * Reports whether the image has an entry point.
   *
   * @return True when it does.
   */
  bool IsExecutable() const override;
  /**
   * Returns the path the image came from.
   *
   * @return The path.
   */
  const std::string& GetPath() const { return path_; }

  /**
   * Returns the lowest guest address of the loaded segments.
   *
   * @return The guest address, 0 before the image is loaded.
   */
  u32 GetBaseAddress() const override { return base_address_; }
  /**
   * Returns the size of the loaded segments.
   *
   * @return The size in bytes.
   */
  u32 GetImageSize() const override { return image_size_; }
  /**
   * Returns the entry point of the image.
   *
   * @return The guest address.
   */
  u32 GetEntryPoint() const override { return entry_point_; }

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
  std::vector<u8> elf_header_mem_;
  u32 entry_point_ = 0;
  u32 base_address_ = 0;
  u32 image_size_ = 0;
};

}  // namespace rex::runtime
