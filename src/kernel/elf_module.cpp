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
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/kernel/module.h>
#include <rex/kernel/function_dispatcher.h>
#include <rex/types.h>

namespace rex::kernel {

ELFModule::ELFModule(FunctionDispatcher* function_dispatcher) : Module(function_dispatcher) {}

ELFModule::~ELFModule() = default;

struct ELF32Ehdr {
  u8 e_ident[16];
  be_u16 e_type;
  be_u16 e_machine;
  be_u32 e_version;
  be_u32 e_entry;
  be_u32 e_phoff;
  be_u32 e_shoff;
  be_u32 e_flags;
  be_u16 e_ehsize;
  be_u16 e_phentsize;
  be_u16 e_phnum;
  be_u16 e_shentsize;
  be_u16 e_shnum;
  be_u16 e_shstrndx;
};

struct ELF32Phdr {
  be_u32 p_type;
  be_u32 p_offset;
  be_u32 p_vaddr;
  be_u32 p_paddr;
  be_u32 p_filesz;
  be_u32 p_memsz;
  be_u32 p_flags;
  be_u32 p_align;
};

bool ELFModule::IsExecutable() const {
  auto hdr = reinterpret_cast<const ELF32Ehdr*>(elf_header_mem_.data());
  return hdr->e_entry != 0;
}

bool ELFModule::Load(const std::string_view name, const std::string_view path, const void* elf_addr,
                     size_t elf_length) {
  name_ = name;
  path_ = path;

  if (elf_length < sizeof(ELF32Ehdr)) {
    return false;
  }

  auto pelf = static_cast<const u8*>(elf_addr);
  auto hdr = reinterpret_cast<const ELF32Ehdr*>(pelf);
  if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || hdr->e_ident[2] != 'L' ||
      hdr->e_ident[3] != 'F') {
    return false;
  }

  if (hdr->e_ident[4] != 1) {
    REXSYS_FAIL(false, "ELF: Could not load ELF because it isn't a 32 bit file!");
  }

  if (hdr->e_type != 2 /* ET_EXEC */) {
    REXSYS_ERROR("ELF: Could not load ELF because it isn't executable!");
    return false;
  }

  if (hdr->e_machine != 20 /* EM_PPC */) {
    REXSYS_FAIL(false,
                "ELF: Could not load ELF because target machine is not PPC! (target: "
                "{})",
                static_cast<u32>(hdr->e_machine));
  }

  if (!hdr->e_phoff) {
    REXSYS_FAIL(false, "ELF: File doesn't have a program header!");
  }

  if (!hdr->e_entry) {
    REXSYS_FAIL(false, "ELF: Executable has no entry point!");
  }

  entry_point_ = hdr->e_entry;

  if (hdr->e_ehsize < sizeof(ELF32Ehdr) || hdr->e_ehsize > elf_length) {
    REXSYS_FAIL(false, "ELF: The ELF header size is invalid!");
  }

  if (hdr->e_phentsize != sizeof(ELF32Phdr)) {
    REXSYS_FAIL(false, "ELF: The program header entries have an unsupported size!");
  }

  if (static_cast<u64>(hdr->e_phoff) + static_cast<u64>(hdr->e_phnum) * sizeof(ELF32Phdr) >
      elf_length) {
    REXSYS_FAIL(false, "ELF: The program header table is outside the file!");
  }

  elf_header_mem_.resize(hdr->e_ehsize);
  std::memcpy(elf_header_mem_.data(), hdr, hdr->e_ehsize);

  auto phdr = reinterpret_cast<const ELF32Phdr*>(pelf + hdr->e_phoff);

  u32 min_addr = UINT32_MAX;
  u32 max_addr = 0;

  for (u32 i = 0; i < hdr->e_phnum; i++) {
    if (phdr[i].p_type == 1 /* PT_LOAD */ || phdr[i].p_type == 2 /* PT_DYNAMIC */) {
      if (phdr[i].p_filesz > phdr[i].p_memsz ||
          static_cast<u64>(phdr[i].p_offset) + phdr[i].p_filesz > elf_length ||
          static_cast<u64>(phdr[i].p_vaddr) + phdr[i].p_memsz > 0xA0000000ull) {
        REXSYS_FAIL(false, "ELF: A segment lies outside the file or the address range!");
      }

      const u32 alignment = std::max<u32>(phdr[i].p_align, 1);
      if (alignment & (alignment - 1)) {
        REXSYS_FAIL(false, "ELF: A segment alignment is not a power of 2!");
      }

      u32 seg_start = phdr[i].p_vaddr;
      u32 seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
      min_addr = std::min(min_addr, seg_start);
      max_addr = std::max(max_addr, seg_end);

      if (phdr[i].p_vaddr < 0x80000000 || phdr[i].p_vaddr > 0x9FFFFFFF) {
        REXSYS_FAIL(false, "ELF: Segment address 0x{:08X} is outside the guest address range",
                    static_cast<u32>(phdr[i].p_vaddr));
      }

      u32 virtual_addr = phdr[i].p_vaddr & ~(alignment - 1);
      u32 virtual_size =
          rex::round_up_nonzero(phdr[i].p_vaddr + phdr[i].p_memsz, alignment) - virtual_addr;
      if (!GetMemory()
               ->LookupHeap(virtual_addr)
               ->AllocFixed(
                   virtual_addr, virtual_size, alignment,
                   rex::kernel::kMemoryAllocationReserve | rex::kernel::kMemoryAllocationCommit,
                   rex::kernel::kMemoryProtectRead | rex::kernel::kMemoryProtectWrite)) {
        REXSYS_ERROR("ELF: Could not allocate memory!");
      }

      auto p = GetMemory()->TranslateVirtual(phdr[i].p_vaddr);
      std::memset(p, 0, phdr[i].p_memsz);
      std::memcpy(p, pelf + phdr[i].p_offset, phdr[i].p_filesz);

      (void)virtual_addr;
      (void)virtual_size;
    }
  }

  base_address_ = (min_addr != UINT32_MAX) ? min_addr : 0;
  image_size_ = (max_addr > min_addr) ? (max_addr - min_addr) : 0;

  loaded_ = true;
  return true;
}

bool ELFModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;
  return true;
}

}  // namespace rex::kernel
