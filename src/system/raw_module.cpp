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

#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/system/function.h>
#include <rex/system/raw_module.h>

namespace rex::runtime {

namespace {
// Simple Function implementation for RawModule symbol tracking
class RawFunction : public Function {
 public:
  RawFunction(Module* module, uint32_t address) : Function(module, address) {}
};
}  // namespace

RawModule::RawModule(Processor* processor)
    : Module(processor), base_address_(0), low_address_(0), high_address_(0) {}

RawModule::~RawModule() {}

bool RawModule::LoadFile(uint32_t base_address, const std::filesystem::path& path) {
  FILE* file = rex::filesystem::OpenFile(path, "rb");
  fseek(file, 0, SEEK_END);
  uint32_t file_length = static_cast<uint32_t>(ftell(file));
  fseek(file, 0, SEEK_SET);

  // Allocate memory.
  // Since we have no real heap just load it wherever.
  base_address_ = base_address;
  auto heap = memory_->LookupHeap(base_address_);
  if (!heap || !heap->AllocFixed(base_address_, file_length, 0,
                                 memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                                 memory::kMemoryProtectRead | memory::kMemoryProtectWrite)) {
    return false;
  }

  uint8_t* p = memory_->TranslateVirtual(base_address_);

  // Read into memory.
  fread(p, file_length, 1, file);
  fclose(file);

  // Setup debug info.
  name_ = rex::path_to_utf8(path.filename());
  // TODO(benvanik): debug info

  low_address_ = base_address;
  high_address_ = base_address + file_length;

  // Populate binary section for introspection
  binary_sections_.clear();
  binary_sections_.push_back(BinarySection{
      ".text", base_address_, file_length, p,
      true,  // executable
      false  // writable
  });

  return true;
}

void RawModule::SetAddressRange(uint32_t base_address, uint32_t size) {
  base_address_ = base_address;
  low_address_ = base_address;
  high_address_ = base_address + size;

  // Populate binary section for introspection
  binary_sections_.clear();
  binary_sections_.push_back(BinarySection{
      ".text", base_address_, size, memory_->TranslateVirtual(base_address_),
      true,  // executable
      false  // writable
  });
}

bool RawModule::ContainsAddress(uint32_t address) {
  return address >= low_address_ && address < high_address_;
}

std::unique_ptr<Function> RawModule::CreateFunction(uint32_t address) {
  return std::make_unique<RawFunction>(this, address);
}

}  // namespace rex::runtime
// (removed orphan xe namespace)
