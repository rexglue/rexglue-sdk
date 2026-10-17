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
#include <rex/fail.h>
#include <rex/logging/log.h>
#include <rex/math.h>
#include <rex/runtime/elf_module.h>
#include <rex/runtime/function_dispatcher.h>
#include <rex/types.h>

namespace rex::runtime {

ELFModule::ELFModule(FunctionDispatcher* function_dispatcher) : Module(function_dispatcher) {}

ELFModule::~ELFModule() = default;

struct ELF32Ehdr {
  u8 e_ident[16];
  be_u16 e_type;
  be_u16 e_machine;
  be_u32 e_version;
  rex::BE<u32> e_entry;
  be_u32 e_phoff;
  be_u32 e_shoff;
  be_u32 e_flags;
  be_u16 e_ehsize;
  be_u16 e_phentsize;
  be_u16 e_phnum;
  be_u16 e_shentsize;
  be_u16 e_shnum;
  be_u16 e_shtrndx;
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
  (void)elf_length;

  auto pelf = static_cast<const u8*>(elf_addr);
  auto hdr = reinterpret_cast<const ELF32Ehdr*>(pelf);
  if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || hdr->e_ident[2] != 'L' ||
      hdr->e_ident[3] != 'F') {
    return false;
  }

  REX_ASSERT(hdr->e_ident[4] == 1);

  if (hdr->e_type != 2 /* ET_EXEC */) {
    REXLOG_ERROR(system, "ELF: Could not load ELF because it isn't executable!");
    return false;
  }

  if (hdr->e_machine != 20 /* EM_PPC */) {
    REX_FAIL(system, false,
             "ELF: Could not load ELF because target machine is not PPC! (target: "
             "{})",
             static_cast<u32>(hdr->e_machine));
  }

  if (!hdr->e_phoff) {
    REX_FAIL(system, false, "ELF: File doesn't have a program header!");
  }

  if (!hdr->e_entry) {
    REX_FAIL(system, false, "ELF: Executable has no entry point!");
  }

  entry_point_ = hdr->e_entry;

  elf_header_mem_.resize(hdr->e_ehsize);
  std::memcpy(elf_header_mem_.data(), hdr, hdr->e_ehsize);

  REX_ASSERT(hdr->e_phentsize == sizeof(ELF32Phdr));
  auto phdr = reinterpret_cast<const ELF32Phdr*>(pelf + hdr->e_phoff);

  u32 min_addr = UINT32_MAX;
  u32 max_addr = 0;

  for (u32 i = 0; i < hdr->e_phnum; i++) {
    if (phdr[i].p_type == 1 /* PT_LOAD */ || phdr[i].p_type == 2 /* PT_DYNAMIC */) {
      u32 seg_start = phdr[i].p_vaddr;
      u32 seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
      min_addr = std::min(min_addr, seg_start);
      max_addr = std::max(max_addr, seg_end);

      if (phdr[i].p_vaddr < 0x80000000 || phdr[i].p_vaddr > 0x9FFFFFFF) {
        REX_FAIL(system, false, "ELF: Could not allocate memory for section @ address 0x{:08X}",
                 static_cast<u32>(phdr[i].p_vaddr));
      }

      u32 virtual_addr = phdr[i].p_vaddr & ~(alignment - 1);
      u32 virtual_size =
          rex::RoundUpNonZero(phdr[i].p_vaddr + phdr[i].p_memsz, alignment) - virtual_addr;
      if (!GetMemory()
               ->LookupHeap(virtual_addr)
               ->AllocFixed(
                   virtual_addr, virtual_size, phdr[i].p_align,
                   rex::runtime::kMemoryAllocationReserve | rex::runtime::kMemoryAllocationCommit,
                   rex::runtime::kMemoryProtectRead | rex::runtime::kMemoryProtectWrite)) {
        REXLOG_ERROR(system, "ELF: Could not allocate memory!");
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

}  // namespace rex::runtime
