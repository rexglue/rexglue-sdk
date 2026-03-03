/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <atomic>
#include <algorithm>
#include <cstring>
#include <utility>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <unistd.h>
#endif

// Signal-safe logging helpers for ExceptionCallback, which may run inside a
// POSIX signal handler where stdio locks are unsafe.
namespace {

void mmio_ss_write(const char* s) {
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  if (!s) {
    return;
  }
  size_t length = 0;
  while (s[length]) {
    ++length;
  }
  if (length) {
    write(STDERR_FILENO, s, length);
  }
#else
  (void)s;
#endif
}

void mmio_ss_hex(uint64_t value) {
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  char buffer[18];
  buffer[0] = '0';
  buffer[1] = 'x';
  for (int i = 17; i >= 2; --i) {
    buffer[i] = "0123456789abcdef"[value & 0xF];
    value >>= 4;
  }
  write(STDERR_FILENO, buffer, sizeof(buffer));
#else
  (void)value;
#endif
}

void mmio_ss_dec(uint32_t value) {
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  char buffer[10];
  int index = 10;
  if (!value) {
    buffer[--index] = '0';
  } else {
    while (value && index) {
      buffer[--index] = static_cast<char>('0' + (value % 10));
      value /= 10;
    }
  }
  write(STDERR_FILENO, buffer + index, 10 - index);
#else
  (void)value;
#endif
}

}  // namespace

#include <rex/assert.h>
#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/platform.h>
#include <rex/system/mmio_handler.h>
#include <rex/types.h>

namespace rex::runtime {

#if REX_ARCH_ARM64
using namespace rex::arch;
#endif

MMIOHandler* MMIOHandler::global_handler_ = nullptr;

std::unique_ptr<MMIOHandler> MMIOHandler::Install(uint8_t* virtual_membase,
                                                  uint8_t* physical_membase, uint8_t* membase_end,
                                                  HostToGuestVirtual host_to_guest_virtual,
                                                  const void* host_to_guest_virtual_context,
                                                  AccessViolationCallback access_violation_callback,
                                                  void* access_violation_callback_context) {
  // There can be only one handler at a time.
  assert_null(global_handler_);
  if (global_handler_) {
    return nullptr;
  }

  auto handler = std::unique_ptr<MMIOHandler>(new MMIOHandler(
      virtual_membase, physical_membase, membase_end, host_to_guest_virtual,
      host_to_guest_virtual_context, access_violation_callback, access_violation_callback_context));

  // Install exception handler for memory coherence (SharedMemory write tracking).
  // Note: MMIO operations are handled at the recompiler level via PPC_MM_LOAD/STORE
  // macros that call CheckLoad/CheckStore directly.
  arch::ExceptionHandler::Install(ExceptionCallbackThunk, handler.get());

  global_handler_ = handler.get();
  return handler;
}

MMIOHandler::MMIOHandler(uint8_t* virtual_membase, uint8_t* physical_membase, uint8_t* membase_end,
                         HostToGuestVirtual host_to_guest_virtual,
                         const void* host_to_guest_virtual_context,
                         AccessViolationCallback access_violation_callback,
                         void* access_violation_callback_context)
    : virtual_membase_(virtual_membase),
      physical_membase_(physical_membase),
      memory_end_(membase_end),
      host_to_guest_virtual_(host_to_guest_virtual),
      host_to_guest_virtual_context_(host_to_guest_virtual_context),
      access_violation_callback_(access_violation_callback),
      access_violation_callback_context_(access_violation_callback_context) {}

MMIOHandler::~MMIOHandler() {
  arch::ExceptionHandler::Uninstall(ExceptionCallbackThunk, this);

  assert_true(global_handler_ == this);
  global_handler_ = nullptr;
}

bool MMIOHandler::RegisterRange(uint32_t virtual_address, uint32_t mask, uint32_t size,
                                void* context, MMIOReadCallback read_callback,
                                MMIOWriteCallback write_callback) {
  mapped_ranges_.push_back({
      virtual_address,
      mask,
      size,
      context,
      read_callback,
      write_callback,
  });
  return true;
}

MMIORange* MMIOHandler::LookupRange(uint32_t virtual_address) {
  for (auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      return &range;
    }
  }
  return nullptr;
}

bool MMIOHandler::CheckLoad(uint32_t virtual_address, uint32_t* out_value) {
  for (const auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      *out_value =
          static_cast<uint32_t>(range.read(nullptr, range.callback_context, virtual_address));
      return true;
    }
  }
  return false;
}

bool MMIOHandler::CheckStore(uint32_t virtual_address, uint32_t value) {
  for (const auto& range : mapped_ranges_) {
    if ((virtual_address & range.mask) == range.address) {
      range.write(nullptr, range.callback_context, virtual_address, value);
      return true;
    }
  }
  return false;
}

bool MMIOHandler::TryDecodeLoadStore(const uint8_t* p, DecodedLoadStore& decoded_out) {
  std::memset(&decoded_out, 0, sizeof(decoded_out));
#if REX_ARCH_AMD64
  uint8_t i = 0;  // Current byte decode index.
  uint8_t rex = 0;
  if ((p[i] & 0xF0) == 0x40) {
    rex = p[0];
    ++i;
  }
  if (p[i] == 0x0F && p[i + 1] == 0x38 && p[i + 2] == 0xF1) {
    // MOVBE m32, r32 (store)
    // https://web.archive.org/web/20170629091435/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOVBE.html
    // 44 0f 38 f1 a4 02 00     movbe  DWORD PTR [rdx+rax*1+0x0],r12d
    // 42 0f 38 f1 8c 22 00     movbe  DWORD PTR [rdx+r12*1+0x0],ecx
    // 0f 38 f1 8c 02 00 00     movbe  DWORD PTR [rdx + rax * 1 + 0x0], ecx
    decoded_out.is_load = false;
    decoded_out.byte_swap = true;
    decoded_out.access_size = sizeof(uint32_t);
    i += 3;
  } else if (p[i] == 0x0F && p[i + 1] == 0x38 && p[i + 2] == 0xF0) {
    // MOVBE r32, m32 (load)
    // https://web.archive.org/web/20170629091435/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOVBE.html
    // 44 0f 38 f0 a4 02 00     movbe  r12d,DWORD PTR [rdx+rax*1+0x0]
    // 42 0f 38 f0 8c 22 00     movbe  ecx,DWORD PTR [rdx+r12*1+0x0]
    // 46 0f 38 f0 a4 22 00     movbe  r12d,DWORD PTR [rdx+r12*1+0x0]
    // 0f 38 f0 8c 02 00 00     movbe  ecx,DWORD PTR [rdx+rax*1+0x0]
    // 0F 38 F0 1C 02           movbe  ebx,dword ptr [rdx+rax]
    decoded_out.is_load = true;
    decoded_out.byte_swap = true;
    decoded_out.access_size = sizeof(uint32_t);
    i += 3;
  } else if (p[i] == 0x89) {
    // MOV m32, r32 (store)
    // https://web.archive.org/web/20170629072136/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOV.html
    // 44 89 24 02              mov  DWORD PTR[rdx + rax * 1], r12d
    // 42 89 0c 22              mov  DWORD PTR[rdx + r12 * 1], ecx
    // 89 0c 02                 mov  DWORD PTR[rdx + rax * 1], ecx
    decoded_out.is_load = false;
    decoded_out.byte_swap = false;
    decoded_out.access_size = sizeof(uint32_t);
    ++i;
  } else if (p[i] == 0x8B) {
    // MOV r32, m32 (load)
    // https://web.archive.org/web/20170629072136/https://www.tptp.cc/mirrors/siyobik.info/instruction/MOV.html
    // 44 8b 24 02              mov  r12d, DWORD PTR[rdx + rax * 1]
    // 42 8b 0c 22              mov  ecx, DWORD PTR[rdx + r12 * 1]
    // 46 8b 24 22              mov  r12d, DWORD PTR[rdx + r12 * 1]
    // 8b 0c 02                 mov  ecx, DWORD PTR[rdx + rax * 1]
    decoded_out.is_load = true;
    decoded_out.byte_swap = false;
    decoded_out.access_size = sizeof(uint32_t);
    ++i;
  } else if (p[i] == 0xC7) {
    // MOV m32, simm32
    // https://web.archive.org/web/20161017042413/https://www.asmpedia.org/index.php?title=MOV
    // C7 04 02 02 00 00 00     mov  dword ptr [rdx+rax],2
    decoded_out.is_load = false;
    decoded_out.byte_swap = false;
    decoded_out.is_constant = true;
    decoded_out.access_size = sizeof(uint32_t);
    ++i;
  } else {
    return false;
  }

  uint8_t rex_b = rex & 0b0001;
  uint8_t rex_x = rex & 0b0010;
  uint8_t rex_r = rex & 0b0100;
  uint8_t rex_w = rex & 0b1000;

  // http://www.sandpile.org/x86/opc_rm.htm
  // http://www.sandpile.org/x86/opc_sib.htm
  uint8_t modrm = p[i++];
  uint8_t mod = (modrm & 0b11000000) >> 6;
  uint8_t reg = (modrm & 0b00111000) >> 3;
  uint8_t rm = (modrm & 0b00000111);
  decoded_out.value_reg = reg + (rex_r ? 8 : 0);
  decoded_out.mem_has_base = false;
  decoded_out.mem_base_reg = 0;
  decoded_out.mem_has_index = false;
  decoded_out.mem_index_reg = 0;
  decoded_out.mem_scale = 1;
  decoded_out.mem_displacement = 0;
  bool has_sib = false;
  switch (rm) {
    case 0b100:  // SIB
      has_sib = true;
      break;
    case 0b101:
      if (mod == 0b00) {
        // RIP-relative not supported.
        return false;
      }
      decoded_out.mem_has_base = true;
      decoded_out.mem_base_reg = rm + (rex_b ? 8 : 0);
      break;
    default:
      decoded_out.mem_has_base = true;
      decoded_out.mem_base_reg = rm + (rex_b ? 8 : 0);
      break;
  }
  if (has_sib) {
    uint8_t sib = p[i++];
    decoded_out.mem_scale = 1 << ((sib & 0b11000000) >> 8);
    uint8_t sib_index = (sib & 0b00111000) >> 3;
    uint8_t sib_base = (sib & 0b00000111);
    switch (sib_index) {
      case 0b100:
        // No index.
        break;
      default:
        decoded_out.mem_has_index = true;
        decoded_out.mem_index_reg = sib_index + (rex_x ? 8 : 0);
        decoded_out.mem_index_size = sizeof(uint64_t);
        break;
    }
    switch (sib_base) {
      case 0b101:
        // Alternate rbp-relative addressing not supported.
        assert_zero(mod);
        return false;
      default:
        decoded_out.mem_has_base = true;
        decoded_out.mem_base_reg = sib_base + (rex_b ? 8 : 0);
        break;
    }
  }
  switch (mod) {
    case 0b00: {
      decoded_out.mem_displacement += 0;
    } break;
    case 0b01: {
      decoded_out.mem_displacement += int8_t(p[i++]);
    } break;
    case 0b10: {
      decoded_out.mem_displacement += memory::load<int32_t>(p + i);
      i += 4;
    } break;
  }
  if (decoded_out.is_constant) {
    decoded_out.constant = memory::load<int32_t>(p + i);
    i += 4;
  }
  decoded_out.length = i;
  return true;

#elif REX_ARCH_ARM64
  decoded_out.length = sizeof(uint32_t);
  uint32_t instruction = *reinterpret_cast<const uint32_t*>(p);

  // Literal loading (PC-relative) is not handled.

  if ((instruction & kArm64LoadStoreAnyFMask) != kArm64LoadStoreAnyFixed) {
    // Not a load or a store instruction.
    return false;
  }

  if ((instruction & kArm64LoadStorePairAnyFMask) == kArm64LoadStorePairAnyFixed) {
    // Handling MMIO only for single 32-bit values, not for pairs.
    return false;
  }

  uint8_t value_reg_base;
  switch (Arm64LoadStoreOp(instruction & kArm64LoadStoreMask)) {
    case Arm64LoadStoreOp::kSTRB_w:
      decoded_out.is_load = false;
      decoded_out.access_size = 1;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDRB_w:
      decoded_out.is_load = true;
      decoded_out.access_size = 1;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kSTRH_w:
      decoded_out.is_load = false;
      decoded_out.access_size = 2;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDRH_w:
      decoded_out.is_load = true;
      decoded_out.access_size = 2;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kSTR_w:
      decoded_out.is_load = false;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      decoded_out.access_size = 4;
      break;
    case Arm64LoadStoreOp::kLDR_w:
      decoded_out.is_load = true;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      decoded_out.access_size = 4;
      break;
    case Arm64LoadStoreOp::kSTR_x:
      decoded_out.is_load = false;
      decoded_out.access_size = 8;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDR_x:
      decoded_out.is_load = true;
      decoded_out.access_size = 8;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDRSB_x:
    case Arm64LoadStoreOp::kLDRSB_w:
      decoded_out.is_load = true;
      decoded_out.access_size = 1;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDRSH_x:
    case Arm64LoadStoreOp::kLDRSH_w:
      decoded_out.is_load = true;
      decoded_out.access_size = 2;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kLDRSW_x:
      decoded_out.is_load = true;
      decoded_out.access_size = 4;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    case Arm64LoadStoreOp::kSTR_b:
      decoded_out.is_load = false;
      decoded_out.access_size = 1;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kLDR_b:
      decoded_out.is_load = true;
      decoded_out.access_size = 1;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kSTR_h:
      decoded_out.is_load = false;
      decoded_out.access_size = 2;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kLDR_h:
      decoded_out.is_load = true;
      decoded_out.access_size = 2;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kSTR_s:
      decoded_out.is_load = false;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      decoded_out.access_size = 4;
      break;
    case Arm64LoadStoreOp::kLDR_s:
      decoded_out.is_load = true;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      decoded_out.access_size = 4;
      break;
    case Arm64LoadStoreOp::kSTR_d:
      decoded_out.is_load = false;
      decoded_out.access_size = 8;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kLDR_d:
      decoded_out.is_load = true;
      decoded_out.access_size = 8;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kSTR_q:
      decoded_out.is_load = false;
      decoded_out.access_size = 16;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kLDR_q:
      decoded_out.is_load = true;
      decoded_out.access_size = 16;
      value_reg_base = DecodedLoadStore::kArm64ValueRegV0;
      break;
    case Arm64LoadStoreOp::kPRFM:
      decoded_out.is_load = true;
      decoded_out.is_constant = true;
      decoded_out.constant = 0;
      decoded_out.access_size = 8;
      value_reg_base = DecodedLoadStore::kArm64ValueRegX0;
      break;
    default:
      return false;
  }

  // `Rt` field (load / store register).
  decoded_out.value_reg = value_reg_base + (instruction & 31);
  if (decoded_out.is_load && decoded_out.value_reg == DecodedLoadStore::kArm64ValueRegZero) {
    // Zero constant rather than a register read.
    decoded_out.is_constant = true;
    decoded_out.constant = 0;
  }

  decoded_out.mem_has_base = true;
  // The base is Xn (for 0...30) or SP (for 31).
  // `Rn` field (first source register).
  decoded_out.mem_base_reg = (instruction >> 5) & 31;

  bool is_unsigned_offset =
      (instruction & kArm64LoadStoreUnsignedOffsetFMask) == kArm64LoadStoreUnsignedOffsetFixed;
  if (is_unsigned_offset) {
    // LDR|STR Wt|St, [Xn|SP{, #pimm}]
    uint32_t unsigned_offset = (instruction >> 10) & 4095;
    decoded_out.mem_displacement = ptrdiff_t(decoded_out.access_size * unsigned_offset);
  } else {
    Arm64LoadStoreOffsetFixed offset =
        Arm64LoadStoreOffsetFixed(instruction & kArm64LoadStoreOffsetFMask);
    int32_t signed_offset = int32_t(instruction << (32 - (9 + 12))) >> (32 - 9);
    switch (offset) {
      case Arm64LoadStoreOffsetFixed::kUnscaledOffset: {
        decoded_out.mem_displacement = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kPostIndex: {
        decoded_out.mem_base_writeback = true;
        decoded_out.mem_base_writeback_offset = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kPreIndex: {
        decoded_out.mem_base_writeback = true;
        decoded_out.mem_base_writeback_offset = signed_offset;
        decoded_out.mem_displacement = signed_offset;
      } break;
      case Arm64LoadStoreOffsetFixed::kRegisterOffset: {
        decoded_out.mem_index_reg = (instruction >> 16) & 31;
        if (decoded_out.mem_index_reg != DecodedLoadStore::kArm64RegZero) {
          decoded_out.mem_has_index = true;
          uint32_t extend_mode = (instruction >> 13) & 0b111;
          if (!(extend_mode & 0b010)) {
            return false;
          }
          decoded_out.mem_index_size = (extend_mode & 0b001) ? sizeof(uint64_t) : sizeof(uint32_t);
          decoded_out.mem_index_sign_extend = (extend_mode & 0b100) != 0;
          decoded_out.mem_scale = (instruction & (UINT32_C(1) << 12)) ? decoded_out.access_size : 1;
        }
      } break;
      default:
        return false;
    }
  }

  return true;

#else
#error TryDecodeLoadStore not implemented for the target CPU architecture.
  return false;
#endif  // REX_ARCH
}

bool MMIOHandler::ExceptionCallbackThunk(arch::Exception* ex, void* data) {
  return reinterpret_cast<MMIOHandler*>(data)->ExceptionCallback(ex);
}

bool MMIOHandler::ExceptionCallback(arch::Exception* ex) {
  if (ex->code() != arch::Exception::Code::kAccessViolation) {
    return false;
  }
  arch::Exception::AccessViolationOperation operation = ex->access_violation_operation();
  if (operation != arch::Exception::AccessViolationOperation::kRead &&
      operation != arch::Exception::AccessViolationOperation::kWrite) {
    // Data Execution Prevention or something else uninteresting.
    return false;
  }
  bool is_write = operation == arch::Exception::AccessViolationOperation::kWrite;
  {
    static std::atomic<uint32_t> s_diag_count{0};
    uint32_t count = s_diag_count.fetch_add(1, std::memory_order_relaxed);
    if (count < 10) {
      mmio_ss_write("[MMIO-DIAG] fault=");
      mmio_ss_hex(ex->fault_address());
      mmio_ss_write(" PC=");
      mmio_ss_hex(ex->pc());
      mmio_ss_write(is_write ? " WR" : " RD");
      mmio_ss_write(" ranges=");
      mmio_ss_dec(static_cast<uint32_t>(mapped_ranges_.size()));
      mmio_ss_write(" #");
      mmio_ss_dec(count + 1);
      mmio_ss_write("\n");
    }
  }
  if (ex->fault_address() < uint64_t(virtual_membase_) ||
      ex->fault_address() > uint64_t(memory_end_)) {
    // Quick kill anything outside our mapping.
    return false;
  }
  void* fault_host_address = reinterpret_cast<void*>(ex->fault_address());

  // Access violations are pretty rare, so we can do a linear search here.
  // Only check if in the virtual range, as we only support virtual ranges.
  const MMIORange* range = nullptr;
  uint32_t fault_guest_virtual_address = 0;
  if (ex->fault_address() < uint64_t(physical_membase_)) {
    fault_guest_virtual_address =
        host_to_guest_virtual_(host_to_guest_virtual_context_, fault_host_address);
    for (const auto& test_range : mapped_ranges_) {
      if ((fault_guest_virtual_address & test_range.mask) == test_range.address) {
        // Address is within the range of this mapping.
        range = &test_range;
        break;
      }
    }
  }
  if (!range) {
    if (mapped_ranges_.empty()) {
      auto rip = ex->pc();
      auto p = reinterpret_cast<const uint8_t*>(rip);
      DecodedLoadStore decoded_load_store;
      if (TryDecodeLoadStore(p, decoded_load_store)) {
        if (decoded_load_store.is_load) {
          static constexpr uint32_t kFaultBudget = 1000;
          struct FaultBudgetState {
            uint64_t pc;
            uint32_t count;
          };
          static thread_local FaultBudgetState s_last_fault = {};
          uint64_t value_to_set = 0;
          if (s_last_fault.pc == rip) {
            ++s_last_fault.count;
            if (s_last_fault.count > kFaultBudget) {
              value_to_set = UINT64_MAX;
              if (s_last_fault.count == kFaultBudget + 1) {
                mmio_ss_write("[MMIO-BUDGET] Exceeded budget at PC=");
                mmio_ss_hex(rip);
                mmio_ss_write(" returning all-ones\n");
              }
            }
          } else {
            s_last_fault = {rip, 1};
          }
#if REX_ARCH_AMD64
          ex->ModifyIntRegister(decoded_load_store.value_reg) = value_to_set;
#elif REX_ARCH_ARM64
          if (decoded_load_store.value_reg >= DecodedLoadStore::kArm64ValueRegX0 &&
              decoded_load_store.value_reg <= (DecodedLoadStore::kArm64ValueRegX0 + 30)) {
            ex->ModifyXRegister(decoded_load_store.value_reg -
                                DecodedLoadStore::kArm64ValueRegX0) = value_to_set;
          } else if (decoded_load_store.value_reg >= DecodedLoadStore::kArm64ValueRegV0 &&
                     decoded_load_store.value_reg <=
                         (DecodedLoadStore::kArm64ValueRegV0 + 31)) {
            auto& v = ex->ModifyVRegister(decoded_load_store.value_reg -
                                          DecodedLoadStore::kArm64ValueRegV0);
            v.low = 0;
            v.high = 0;
            switch (decoded_load_store.access_size) {
              case 1:
                v.u8[0] = static_cast<uint8_t>(value_to_set);
                break;
              case 2:
                v.u16[0] = static_cast<uint16_t>(value_to_set);
                break;
              case 4:
                v.u32[0] = static_cast<uint32_t>(value_to_set);
                break;
              case 8:
                v.u64[0] = value_to_set;
                break;
              case 16:
                v.u64[0] = value_to_set;
                v.u64[1] = value_to_set;
                break;
              default:
                v.u32[0] = static_cast<uint32_t>(value_to_set);
                break;
            }
          }
#endif
        }
#if REX_ARCH_ARM64
        if (decoded_load_store.mem_has_base && decoded_load_store.mem_base_writeback) {
          uintptr_t mem_base_writeback_address = 0;
          arch::HostThreadContext& thread_context = *ex->thread_context();
          if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
            mem_base_writeback_address = thread_context.sp;
          } else {
            mem_base_writeback_address = thread_context.x[decoded_load_store.mem_base_reg];
          }
          mem_base_writeback_address += decoded_load_store.mem_base_writeback_offset;
          if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
            thread_context.sp = mem_base_writeback_address;
          } else {
            ex->ModifyXRegister(decoded_load_store.mem_base_reg) = mem_base_writeback_address;
          }
        }
#endif
        static std::atomic<uint32_t> s_skip_count{0};
        uint32_t count = s_skip_count.fetch_add(1, std::memory_order_relaxed);
        if (count < 20) {
          mmio_ss_write("[MMIO-SKIP] ");
          mmio_ss_write(decoded_load_store.is_load ? "LOAD" : "STORE");
          mmio_ss_write(" PC=");
          mmio_ss_hex(rip);
          mmio_ss_write(" fault=");
          mmio_ss_hex(reinterpret_cast<uint64_t>(fault_host_address));
          mmio_ss_write(" #");
          mmio_ss_dec(count + 1);
          mmio_ss_write("\n");
        }
        ex->set_resume_pc(rip + decoded_load_store.length);
        return true;
      }
      mmio_ss_write("[MMIO-SKIP] DECODE FAIL PC=");
      mmio_ss_hex(rip);
      mmio_ss_write(" fault=");
      mmio_ss_hex(reinterpret_cast<uint64_t>(fault_host_address));
      mmio_ss_write("\n");
      return false;
    }
    // Recheck if the pages are still protected (race condition - another thread
    // clears the watch we just hit).
    // Do this under the lock so we don't introduce another race condition.
    auto lock = global_critical_region_.Acquire();
    memory::PageAccess cur_access = memory::PageAccess::kNoAccess;
    size_t page_length = memory::page_size();
    memory::QueryProtect(fault_host_address, page_length, cur_access);
    if (cur_access != memory::PageAccess::kNoAccess &&
        (!is_write || cur_access != memory::PageAccess::kReadOnly)) {
      // Another thread has cleared this watch. Abort.
      return true;
    }
    // The address is not found within any range, so either a write watch or an
    // actual access violation.
    if (access_violation_callback_) {
      return access_violation_callback_(std::move(lock), access_violation_callback_context_,
                                        fault_host_address, is_write);
    }
    return false;
  }

  auto rip = ex->pc();
  auto p = reinterpret_cast<const uint8_t*>(rip);
  DecodedLoadStore decoded_load_store;
  if (!TryDecodeLoadStore(p, decoded_load_store)) {
    REXLOG_ERROR("Unable to decode MMIO load or store instruction at {:p}",
                 static_cast<const void*>(p));
    assert_always("Unknown MMIO instruction type");
    return false;
  }
  if (decoded_load_store.access_size != sizeof(uint32_t)) {
    REXLOG_ERROR("Unsupported MMIO access size {} at {:p}", decoded_load_store.access_size,
                 static_cast<const void*>(p));
    return false;
  }

  arch::HostThreadContext& thread_context = *ex->thread_context();

#if REX_ARCH_ARM64
  // Preserve the base address with the pre- or the post-index offset to write
  // it after writing the result (since the base address register and the
  // register to load to may be the same, in which case it should receive the
  // original base address with the offset).
  uintptr_t mem_base_writeback_address = 0;
  if (decoded_load_store.mem_has_base && decoded_load_store.mem_base_writeback) {
    if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
      mem_base_writeback_address = thread_context.sp;
    } else {
      assert_true(decoded_load_store.mem_base_reg <= 30);
      mem_base_writeback_address = thread_context.x[decoded_load_store.mem_base_reg];
    }
    mem_base_writeback_address += decoded_load_store.mem_base_writeback_offset;
  }
#endif  // REX_ARCH_ARM64

  uint8_t value_reg = decoded_load_store.value_reg;
  if (decoded_load_store.is_load) {
    // Load of a memory value - read from range, swap, and store in the
    // register.
    uint32_t value = range->read(nullptr, range->callback_context, fault_guest_virtual_address);
    if (!decoded_load_store.byte_swap) {
      // We swap only if it's not a movbe, as otherwise we are swapping twice.
      value = rex::byte_swap(value);
    }
#if REX_ARCH_AMD64
    ex->ModifyIntRegister(value_reg) = value;
#elif REX_ARCH_ARM64
    if (value_reg >= DecodedLoadStore::kArm64ValueRegX0 &&
        value_reg <= (DecodedLoadStore::kArm64ValueRegX0 + 30)) {
      ex->ModifyXRegister(value_reg - DecodedLoadStore::kArm64ValueRegX0) = value;
    } else if (value_reg >= DecodedLoadStore::kArm64ValueRegV0 &&
               value_reg <= (DecodedLoadStore::kArm64ValueRegV0 + 31)) {
      ex->ModifyVRegister(value_reg - DecodedLoadStore::kArm64ValueRegV0).u32[0] = value;
    } else {
      assert_true(value_reg == DecodedLoadStore::kArm64ValueRegZero);
      // Register write is ignored for X31.
    }
#else
#error Register value writing not implemented for the target CPU architecture.
#endif  // REX_ARCH
  } else {
    // Store of a register value - read register, swap, write to range.
    uint32_t value;
    if (decoded_load_store.is_constant) {
      value = uint32_t(decoded_load_store.constant);
    } else {
#if REX_ARCH_AMD64
      value = uint32_t(thread_context.int_registers[value_reg]);
#elif REX_ARCH_ARM64
      if (value_reg >= DecodedLoadStore::kArm64ValueRegX0 &&
          value_reg <= (DecodedLoadStore::kArm64ValueRegX0 + 30)) {
        value = uint32_t(thread_context.x[value_reg - DecodedLoadStore::kArm64ValueRegX0]);
      } else if (value_reg >= DecodedLoadStore::kArm64ValueRegV0 &&
                 value_reg <= (DecodedLoadStore::kArm64ValueRegV0 + 31)) {
        value = thread_context.v[value_reg - DecodedLoadStore::kArm64ValueRegV0].u32[0];
      } else {
        assert_true(value_reg == DecodedLoadStore::kArm64ValueRegZero);
        value = 0;
      }
#else
#error Register value reading not implemented for the target CPU architecture.
#endif  // REX_ARCH
      if (!decoded_load_store.byte_swap) {
        // We swap only if it's not a movbe, as otherwise we are swapping twice.
        value = rex::byte_swap(value);
      }
    }
    range->write(nullptr, range->callback_context, fault_guest_virtual_address, value);
  }

#if REX_ARCH_ARM64
  // Write the base address with the pre- or the post-index offset, overwriting
  // the register to load to if it's the same.
  if (decoded_load_store.mem_has_base && decoded_load_store.mem_base_writeback) {
    if (decoded_load_store.mem_base_reg == DecodedLoadStore::kArm64MemBaseRegSp) {
      thread_context.sp = mem_base_writeback_address;
    } else {
      assert_true(decoded_load_store.mem_base_reg <= 30);
      ex->ModifyXRegister(decoded_load_store.mem_base_reg) = mem_base_writeback_address;
    }
  }
#endif  // REX_ARCH_ARM64

  // Advance RIP to the next instruction so that we resume properly.
  ex->set_resume_pc(rip + decoded_load_store.length);

  return true;
}

}  // namespace rex::runtime
