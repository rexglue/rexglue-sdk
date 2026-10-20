/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless an XSI feature level is
// requested before <ucontext.h> is included.
#define _XOPEN_SOURCE 700
#endif

#include <rex/exception_handler.h>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <signal.h>

#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>

#include <ucontext.h>
#include <unistd.h>

namespace rex::arch {

bool signal_handlers_installed_ = false;
struct sigaction original_sigill_handler_;
struct sigaction original_sigsegv_handler_;
#if REX_PLATFORM_MAC
struct sigaction original_sigbus_handler_;
#endif

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
#if REX_PLATFORM_MAC
  // Darwin's mcontext_t is already a pointer (__darwin_mcontext64*); copy it.
  mcontext_t mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#else
  // Linux's mcontext_t is a large struct; bind by reference to avoid copying it
  // inside the signal handler.
  mcontext_t& mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#endif

  HostThreadContext thread_context;

#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
  thread_context.rip = uint64_t(mcontext->__ss.__rip);
  thread_context.eflags = uint32_t(mcontext->__ss.__rflags);
  // TODO (Graine25): Double check this one for me
  thread_context.rax = uint64_t(mcontext->__ss.__rax);
  thread_context.rcx = uint64_t(mcontext->__ss.__rcx);
  thread_context.rdx = uint64_t(mcontext->__ss.__rdx);
  thread_context.rbx = uint64_t(mcontext->__ss.__rbx);
  thread_context.rsp = uint64_t(mcontext->__ss.__rsp);
  thread_context.rbp = uint64_t(mcontext->__ss.__rbp);
  thread_context.rsi = uint64_t(mcontext->__ss.__rsi);
  thread_context.rdi = uint64_t(mcontext->__ss.__rdi);
  thread_context.r8 = uint64_t(mcontext->__ss.__r8);
  thread_context.r9 = uint64_t(mcontext->__ss.__r9);
  thread_context.r10 = uint64_t(mcontext->__ss.__r10);
  thread_context.r11 = uint64_t(mcontext->__ss.__r11);
  thread_context.r12 = uint64_t(mcontext->__ss.__r12);
  thread_context.r13 = uint64_t(mcontext->__ss.__r13);
  thread_context.r14 = uint64_t(mcontext->__ss.__r14);
  thread_context.r15 = uint64_t(mcontext->__ss.__r15);
  std::memcpy(thread_context.xmm_registers, &mcontext->__fs.__fpu_xmm0,
              sizeof(thread_context.xmm_registers));
#else
  thread_context.rip = uint64_t(mcontext.gregs[REG_RIP]);
  thread_context.eflags = uint32_t(mcontext.gregs[REG_EFL]);
  // The REG_ order may be different than the register indices in the
  // instruction encoding.
  thread_context.rax = uint64_t(mcontext.gregs[REG_RAX]);
  thread_context.rcx = uint64_t(mcontext.gregs[REG_RCX]);
  thread_context.rdx = uint64_t(mcontext.gregs[REG_RDX]);
  thread_context.rbx = uint64_t(mcontext.gregs[REG_RBX]);
  thread_context.rsp = uint64_t(mcontext.gregs[REG_RSP]);
  thread_context.rbp = uint64_t(mcontext.gregs[REG_RBP]);
  thread_context.rsi = uint64_t(mcontext.gregs[REG_RSI]);
  thread_context.rdi = uint64_t(mcontext.gregs[REG_RDI]);
  thread_context.r8 = uint64_t(mcontext.gregs[REG_R8]);
  thread_context.r9 = uint64_t(mcontext.gregs[REG_R9]);
  thread_context.r10 = uint64_t(mcontext.gregs[REG_R10]);
  thread_context.r11 = uint64_t(mcontext.gregs[REG_R11]);
  thread_context.r12 = uint64_t(mcontext.gregs[REG_R12]);
  thread_context.r13 = uint64_t(mcontext.gregs[REG_R13]);
  thread_context.r14 = uint64_t(mcontext.gregs[REG_R14]);
  thread_context.r15 = uint64_t(mcontext.gregs[REG_R15]);
  std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm,
              sizeof(thread_context.xmm_registers));
#endif
#elif REX_ARCH_ARM64
#if REX_PLATFORM_MAC
  // macOS ARM64: mcontext_t is __darwin_mcontext64* (pointer, not value)
  std::memcpy(thread_context.x, mcontext->__ss.__x, sizeof(mcontext->__ss.__x));
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
  thread_context.x[29] = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_fp);
  thread_context.x[30] = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_lr);
  thread_context.sp = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_sp);
  thread_context.pc = reinterpret_cast<uintptr_t>(mcontext->__ss.__opaque_pc);
#else
  thread_context.x[29] = mcontext->__ss.__fp;
  thread_context.x[30] = mcontext->__ss.__lr;
  thread_context.sp = mcontext->__ss.__sp;
  thread_context.pc = mcontext->__ss.__pc;
#endif
  thread_context.pstate = mcontext->__ss.__cpsr;
  thread_context.fpsr = mcontext->__ns.__fpsr;
  thread_context.fpcr = mcontext->__ns.__fpcr;
  static_assert(sizeof(thread_context.v) == sizeof(mcontext->__ns.__v));
  std::memcpy(thread_context.v, mcontext->__ns.__v, sizeof(thread_context.v));
#else
  // Linux ARM64: mcontext_t is struct sigcontext (value, not pointer)
  std::memcpy(thread_context.x, mcontext.regs, sizeof(thread_context.x));
  thread_context.sp = mcontext.sp;
  thread_context.pc = mcontext.pc;
  thread_context.pstate = mcontext.pstate;
  struct fpsimd_context* mcontext_fpsimd = nullptr;
  struct esr_context* mcontext_esr = nullptr;
  for (struct _aarch64_ctx* mcontext_extension =
           reinterpret_cast<struct _aarch64_ctx*>(mcontext.__reserved);
       mcontext_extension->magic;
       mcontext_extension = reinterpret_cast<struct _aarch64_ctx*>(
           reinterpret_cast<uint8_t*>(mcontext_extension) + mcontext_extension->size)) {
    switch (mcontext_extension->magic) {
      case FPSIMD_MAGIC:
        mcontext_fpsimd = reinterpret_cast<struct fpsimd_context*>(mcontext_extension);
        break;
      case ESR_MAGIC:
        mcontext_esr = reinterpret_cast<struct esr_context*>(mcontext_extension);
        break;
      default:
        break;
    }
  }
  assert_not_null(mcontext_fpsimd);
  if (mcontext_fpsimd) {
    thread_context.fpsr = mcontext_fpsimd->fpsr;
    thread_context.fpcr = mcontext_fpsimd->fpcr;
    std::memcpy(thread_context.v, mcontext_fpsimd->vregs, sizeof(thread_context.v));
  }
#endif  // REX_PLATFORM_MAC
#endif  // REX_ARCH

  Exception ex;
  switch (signal_number) {
    case SIGILL:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case SIGBUS:
      // On macOS, SIGBUS (KERN_PROTECTION_FAILURE) is raised for writes to
      // read-protected pages — the same scenario that raises SIGSEGV on Linux.
      // Fall through to treat it as an access violation.
#if !REX_PLATFORM_MAC
      // On non-Mac POSIX, SIGBUS is a bus error (misaligned access), not a
      // protection fault — don't handle it here.
      assert_unhandled_case(signal_number);
      return;
#endif
      [[fallthrough]];
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation;
#if REX_ARCH_AMD64
      // x86_pf_error_code::X86_PF_WRITE
      constexpr uint64_t kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
#if REX_PLATFORM_MAC
      access_violation_operation = (uint64_t(mcontext->__es.__err) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#else
      access_violation_operation = (uint64_t(mcontext.gregs[REG_ERR]) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#endif
#elif REX_ARCH_ARM64
      // For a Data Abort (EC - ESR_EL1 bits 31:26 - 0b100100 from a lower
      // Exception Level, 0b100101 without a change in the Exception Level),
      // bit 6 is 0 for reading from a memory location, 1 for writing to a
      // memory location.
#if REX_PLATFORM_MAC
      {
        uint32_t mac_esr = mcontext->__es.__esr;
        if (((mac_esr >> 26) & 0b111110) == 0b100100) {
          access_violation_operation = (mac_esr & (1U << 6))
                                           ? Exception::AccessViolationOperation::kWrite
                                           : Exception::AccessViolationOperation::kRead;
        } else {
          bool instruction_is_store;
          if (IsArm64LoadPrefetchStore(*reinterpret_cast<const uint32_t*>(thread_context.pc),
                                       instruction_is_store)) {
            access_violation_operation = instruction_is_store
                                             ? Exception::AccessViolationOperation::kWrite
                                             : Exception::AccessViolationOperation::kRead;
          } else {
            assert_always(
                "No ESR in the exception thread context, or it's not a Data "
                "Abort, and the faulting instruction is not a known load, "
                "prefetch or store instruction");
            access_violation_operation = Exception::AccessViolationOperation::kUnknown;
          }
        }
      }
#else
      if (mcontext_esr && ((mcontext_esr->esr >> 26) & 0b111110) == 0b100100) {
        access_violation_operation = (mcontext_esr->esr & (UINT64_C(1) << 6))
                                         ? Exception::AccessViolationOperation::kWrite
                                         : Exception::AccessViolationOperation::kRead;
      } else {
        // Determine the memory access direction based on which instruction has
        // requested it.
        // esr_context may be unavailable on certain hosts (for instance, on
        // Android, it was added only in NDK r16 - which is the first NDK
        // version to support the Android API level 27, while NDK r15 doesn't
        // have esr_context in its API 26 sigcontext.h).
        // On AArch64 (unlike on AArch32), the program counter is the address of
        // the currently executing instruction.
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(*reinterpret_cast<const uint32_t*>(mcontext.pc),
                                     instruction_is_store)) {
          access_violation_operation = instruction_is_store
                                           ? Exception::AccessViolationOperation::kWrite
                                           : Exception::AccessViolationOperation::kRead;
        } else {
          assert_always(
              "No ESR in the exception thread context, or it's not a Data "
              "Abort, and the faulting instruction is not a known load, "
              "prefetch or store instruction");
          access_violation_operation = Exception::AccessViolationOperation::kUnknown;
        }
      }
#endif  // REX_PLATFORM_MAC
#else
      access_violation_operation = Exception::AccessViolationOperation::kUnknown;
#endif  // REX_ARCH
      ex.InitializeAccessViolation(&thread_context,
                                   reinterpret_cast<uint64_t>(signal_info->si_addr),
                                   access_violation_operation);
    } break;
    default:
      assert_unhandled_case(signal_number);
  }

  for (size_t i = 0; i < rex::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
      mcontext->__ss.__rip = thread_context.rip;
      mcontext->__ss.__rflags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        switch (modified_register_index) {
          case 0:
            mcontext->__ss.__rax = thread_context.int_registers[modified_register_index];
            break;
          case 1:
            mcontext->__ss.__rcx = thread_context.int_registers[modified_register_index];
            break;
          case 2:
            mcontext->__ss.__rdx = thread_context.int_registers[modified_register_index];
            break;
          case 3:
            mcontext->__ss.__rbx = thread_context.int_registers[modified_register_index];
            break;
          case 4:
            mcontext->__ss.__rsp = thread_context.int_registers[modified_register_index];
            break;
          case 5:
            mcontext->__ss.__rbp = thread_context.int_registers[modified_register_index];
            break;
          case 6:
            mcontext->__ss.__rsi = thread_context.int_registers[modified_register_index];
            break;
          case 7:
            mcontext->__ss.__rdi = thread_context.int_registers[modified_register_index];
            break;
          case 8:
            mcontext->__ss.__r8 = thread_context.int_registers[modified_register_index];
            break;
          case 9:
            mcontext->__ss.__r9 = thread_context.int_registers[modified_register_index];
            break;
          case 10:
            mcontext->__ss.__r10 = thread_context.int_registers[modified_register_index];
            break;
          case 11:
            mcontext->__ss.__r11 = thread_context.int_registers[modified_register_index];
            break;
          case 12:
            mcontext->__ss.__r12 = thread_context.int_registers[modified_register_index];
            break;
          case 13:
            mcontext->__ss.__r13 = thread_context.int_registers[modified_register_index];
            break;
          case 14:
            mcontext->__ss.__r14 = thread_context.int_registers[modified_register_index];
            break;
          case 15:
            mcontext->__ss.__r15 = thread_context.int_registers[modified_register_index];
            break;
          default:
            assert_unhandled_case(modified_register_index);
        }
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(reinterpret_cast<char*>(&mcontext->__fs.__fpu_xmm0) +
                        modified_register_index * sizeof(vec128_t),
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#else
      mcontext.gregs[REG_RIP] = greg_t(thread_context.rip);
      mcontext.gregs[REG_EFL] = greg_t(thread_context.eflags);
      uint32_t modified_register_index;
      // The order must match the order in X64Register.
      static const size_t kIntRegisterMap[] = {
          REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
          REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        mcontext.gregs[kIntRegisterMap[modified_register_index]] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&mcontext.fpregs->_xmm[modified_register_index],
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#endif
#elif REX_ARCH_ARM64
      uint32_t modified_register_index;
      uint32_t modified_x_registers_remaining = ex.modified_x_registers();
#if REX_PLATFORM_MAC
      while (rex::bit_scan_forward(modified_x_registers_remaining, &modified_register_index)) {
        modified_x_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
        if (modified_register_index < 29) {
          mcontext->__ss.__x[modified_register_index] = thread_context.x[modified_register_index];
        } else if (modified_register_index == 29) {
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
          mcontext->__ss.__opaque_fp = reinterpret_cast<void*>(thread_context.x[29]);
#else
          mcontext->__ss.__fp = thread_context.x[29];
#endif
        } else {
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
          mcontext->__ss.__opaque_lr = reinterpret_cast<void*>(thread_context.x[30]);
#else
          mcontext->__ss.__lr = thread_context.x[30];
#endif
        }
      }
#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
      mcontext->__ss.__opaque_sp = reinterpret_cast<void*>(thread_context.sp);
      mcontext->__ss.__opaque_pc = reinterpret_cast<void*>(thread_context.pc);
#else
      mcontext->__ss.__sp = thread_context.sp;
      mcontext->__ss.__pc = thread_context.pc;
#endif
      mcontext->__ss.__cpsr = thread_context.pstate;
      mcontext->__ns.__fpsr = thread_context.fpsr;
      mcontext->__ns.__fpcr = thread_context.fpcr;
      {
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (rex::bit_scan_forward(modified_v_registers_remaining, &modified_register_index)) {
          modified_v_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext->__ns.__v[modified_register_index],
                      &thread_context.v[modified_register_index], sizeof(vec128_t));
        }
      }
#else
      while (rex::bit_scan_forward(modified_x_registers_remaining, &modified_register_index)) {
        modified_x_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
        mcontext.regs[modified_register_index] = thread_context.x[modified_register_index];
      }
      mcontext.sp = thread_context.sp;
      mcontext.pc = thread_context.pc;
      mcontext.pstate = thread_context.pstate;
      if (mcontext_fpsimd) {
        mcontext_fpsimd->fpsr = thread_context.fpsr;
        mcontext_fpsimd->fpcr = thread_context.fpcr;
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (rex::bit_scan_forward(modified_v_registers_remaining, &modified_register_index)) {
          modified_v_registers_remaining &= ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext_fpsimd->vregs[modified_register_index],
                      &thread_context.v[modified_register_index], sizeof(vec128_t));
          mcontext.regs[modified_register_index] = thread_context.x[modified_register_index];
        }
      }
#endif  // REX_PLATFORM_MAC
#endif  // REX_ARCH
      return;
    }
  }

  // No handler claimed the fault (it isn't a guest memory watch): a real
  // crash, such as a null pointer in a graphics driver or a bug. Returning
  // would run the faulting instruction again, fault again, and so on for ever:
  // the thread spins at a full core and nothing is logged, so the crash looks
  // like a hang. Hand the signal to what was installed before this handler
  // (normally the default action: the process ends, with a core dump) by
  // restoring it; the instruction then faults once more under it. A line goes
  // to stderr first (write() is safe in a signal handler).
  {
    char line[160];
    size_t n = 0;
    auto put = [&](const char* s) {
      while (*s && n < sizeof(line) - 1) line[n++] = *s++;
    };
    auto put_hex = [&](uint64_t v) {
      put("0x");
      bool started = false;
      for (int shift = 60; shift >= 0; shift -= 4) {
        const unsigned digit = unsigned(v >> shift) & 0xF;
        if (!digit && !started && shift) continue;
        started = true;
        if (n < sizeof(line) - 1) line[n++] = "0123456789abcdef"[digit];
      }
    };
    put(signal_number == SIGSEGV ? "rex: unhandled SIGSEGV at address "
        : signal_number == SIGBUS ? "rex: unhandled SIGBUS at address "
                                  : "rex: unhandled SIGILL at address ");
    put_hex(reinterpret_cast<uint64_t>(signal_info->si_addr));
    put("; passing it on, the process will end\n");
    [[maybe_unused]] ssize_t written = write(STDERR_FILENO, line, n);
  }
#if REX_PLATFORM_MAC
  const struct sigaction* original = signal_number == SIGSEGV  ? &original_sigsegv_handler_
                                     : signal_number == SIGBUS ? &original_sigbus_handler_
                                                               : &original_sigill_handler_;
#else
  // Only SIGILL and SIGSEGV are taken here off macOS.
  const struct sigaction* original =
      signal_number == SIGSEGV ? &original_sigsegv_handler_ : &original_sigill_handler_;
#endif
  sigaction(signal_number, original, nullptr);
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!signal_handlers_installed_) {
    struct sigaction signal_handler;

    std::memset(&signal_handler, 0, sizeof(signal_handler));
    signal_handler.sa_sigaction = ExceptionHandlerCallback;
    signal_handler.sa_flags = SA_SIGINFO;

    if (sigaction(SIGILL, &signal_handler, &original_sigill_handler_) != 0) {
      assert_always("Failed to install new SIGILL handler");
    }
    if (sigaction(SIGSEGV, &signal_handler, &original_sigsegv_handler_) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
#if REX_PLATFORM_MAC
    if (sigaction(SIGBUS, &signal_handler, &original_sigbus_handler_) != 0) {
      assert_always("Failed to install new SIGBUS handler");
    }
#endif
    signal_handlers_installed_ = true;
  }

  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < rex::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (signal_handlers_installed_) {
      if (sigaction(SIGILL, &original_sigill_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGILL handler");
      }
      if (sigaction(SIGSEGV, &original_sigsegv_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
#if REX_PLATFORM_MAC
      if (sigaction(SIGBUS, &original_sigbus_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
#endif
      signal_handlers_installed_ = false;
    }
  }
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
