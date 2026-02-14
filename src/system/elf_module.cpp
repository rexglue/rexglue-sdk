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

#include <algorithm>
#include <memory>

#include <rex/logging.h>
#include <rex/system/elf_module.h>
#include <rex/system/processor.h>
#include <rex/types.h>

namespace rex::runtime {

ElfModule::ElfModule(Processor* processor, system::KernelState* kernel_state)
    : Module(processor), kernel_state_(kernel_state) {}

ElfModule::~ElfModule() = default;

// ELF structures
struct elf32_ehdr {
  uint8_t e_ident[16];
  rex::be<uint16_t> e_type;
  rex::be<uint16_t> e_machine;
  rex::be<uint32_t> e_version;
  rex::be<uint32_t> e_entry;
  rex::be<uint32_t> e_phoff;
  rex::be<uint32_t> e_shoff;
  rex::be<uint32_t> e_flags;
  rex::be<uint16_t> e_ehsize;
  rex::be<uint16_t> e_phentsize;
  rex::be<uint16_t> e_phnum;
  rex::be<uint16_t> e_shentsize;
  rex::be<uint16_t> e_shnum;
  rex::be<uint16_t> e_shtrndx;
};

struct elf32_phdr {
  rex::be<uint32_t> p_type;
  rex::be<uint32_t> p_offset;
  rex::be<uint32_t> p_vaddr;
  rex::be<uint32_t> p_paddr;
  rex::be<uint32_t> p_filesz;
  rex::be<uint32_t> p_memsz;
  rex::be<uint32_t> p_flags;
  rex::be<uint32_t> p_align;
};

bool ElfModule::is_executable() const {
  auto hdr = reinterpret_cast<const elf32_ehdr*>(elf_header_mem_.data());
  return hdr->e_entry != 0;
}

bool ElfModule::Load(const std::string_view name, const std::string_view path, const void* elf_addr,
                     size_t elf_length) {
  name_ = name;
  path_ = path;

  uint8_t* pelf = (uint8_t*)elf_addr;
  elf32_ehdr* hdr = (elf32_ehdr*)(pelf + 0x0);
  if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || hdr->e_ident[2] != 'L' ||
      hdr->e_ident[3] != 'F') {
    // Not an ELF file!
    return false;
  }

  assert_true(hdr->e_ident[4] == 1);  // 32bit

  if (hdr->e_type != 2 /* ET_EXEC */) {
    // Not executable (shared objects not supported yet)
    REXLOG_ERROR("ELF: Could not load ELF because it isn't executable!");
    return false;
  }

  if (hdr->e_machine != 20 /* EM_PPC */) {
    // Not a PPC ELF!
    REXLOG_ERROR(
        "ELF: Could not load ELF because target machine is not PPC! (target: "
        "{})",
        uint32_t(hdr->e_machine));
    return false;
  }

  // Parse LOAD program headers and load into memory.
  if (!hdr->e_phoff) {
    REXLOG_ERROR("ELF: File doesn't have a program header!");
    return false;
  }

  if (!hdr->e_entry) {
    REXLOG_ERROR("ELF: Executable has no entry point!");
    return false;
  }

  // Entry point virtual address
  entry_point_ = hdr->e_entry;

  // Copy the ELF header
  elf_header_mem_.resize(hdr->e_ehsize);
  std::memcpy(elf_header_mem_.data(), hdr, hdr->e_ehsize);

  assert_true(hdr->e_phentsize == sizeof(elf32_phdr));
  elf32_phdr* phdr = (elf32_phdr*)(pelf + hdr->e_phoff);

  // Calculate base address and image size from loaded segments
  uint32_t min_addr = UINT32_MAX;
  uint32_t max_addr = 0;

  for (uint32_t i = 0; i < hdr->e_phnum; i++) {
    if (phdr[i].p_type == 1 /* PT_LOAD */ || phdr[i].p_type == 2 /* PT_DYNAMIC */) {
      // Track address range
      uint32_t seg_start = phdr[i].p_vaddr;
      uint32_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
      min_addr = std::min(min_addr, seg_start);
      max_addr = std::max(max_addr, seg_end);

      // Allocate and copy into memory.
      // Base address @ 0x80000000
      if (phdr[i].p_vaddr < 0x80000000 || phdr[i].p_vaddr > 0x9FFFFFFF) {
        REXLOG_ERROR("ELF: Could not allocate memory for section @ address 0x{:08X}",
                     uint32_t(phdr[i].p_vaddr));
        return false;
      }

      uint32_t virtual_addr = phdr[i].p_vaddr & ~(phdr[i].p_align - 1);
      uint32_t virtual_size =
          rex::round_up(phdr[i].p_vaddr + phdr[i].p_memsz, uint32_t(phdr[i].p_align)) -
          virtual_addr;
      if (!memory()
               ->LookupHeap(virtual_addr)
               ->AllocFixed(
                   virtual_addr, virtual_size, phdr[i].p_align,
                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
        REXLOG_ERROR("ELF: Could not allocate memory!");
      }

      auto p = memory()->TranslateVirtual(phdr[i].p_vaddr);
      std::memset(p, 0, phdr[i].p_memsz);
      std::memcpy(p, pelf + phdr[i].p_offset, phdr[i].p_filesz);

      // crack: No JIT backend to notify about executable code
      // In JIT mode this would be: processor_->backend()->CommitExecutableRange(...)
      (void)virtual_addr;
      (void)virtual_size;
    }
  }

  // Set base address and image size
  base_address_ = (min_addr != UINT32_MAX) ? min_addr : 0;
  image_size_ = (max_addr > min_addr) ? (max_addr - min_addr) : 0;

  loaded_ = true;
  return true;
}

bool ElfModule::Unload() {
  if (!loaded_) {
    return true;
  }
  // crack: Memory allocated for ELF segments remains - no deallocation
  loaded_ = false;
  return true;
}

std::unique_ptr<Function> ElfModule::CreateFunction(uint32_t address) {
  // crack: No JIT backend - return nullptr (functions are statically compiled)
  (void)address;
  return nullptr;
}

}  // namespace rex::runtime
