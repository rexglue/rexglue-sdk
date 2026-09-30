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
// Darwin exposes ucontext only with a feature level set before <ucontext.h>.
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

namespace rex::platform {

namespace {
bool g_signal_handlers_installed = false;
struct sigaction g_original_sigill_handler;
struct sigaction g_original_sigsegv_handler;
#if REX_PLATFORM_MAC
struct sigaction g_original_sigbus_handler;
#endif

constexpr size_t kMaxHandlerCount = 8;

std::pair<ExceptionHandler::Handler, void*> g_handlers[kMaxHandlerCount];
}  // namespace

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
#if REX_PLATFORM_MAC
  mcontext_t mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#else
  mcontext_t& mcontext = reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#endif

  HostThreadContext thread_context;

#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
  thread_context.rip = u64(mcontext->__ss.__rip);
  thread_context.eflags = u32(mcontext->__ss.__rflags);
  thread_context.rax = u64(mcontext->__ss.__rax);
  thread_context.rcx = u64(mcontext->__ss.__rcx);
  thread_context.rdx = u64(mcontext->__ss.__rdx);
  thread_context.rbx = u64(mcontext->__ss.__rbx);
  thread_context.rsp = u64(mcontext->__ss.__rsp);
  thread_context.rbp = u64(mcontext->__ss.__rbp);
  thread_context.rsi = u64(mcontext->__ss.__rsi);
  thread_context.rdi = u64(mcontext->__ss.__rdi);
  thread_context.r8 = u64(mcontext->__ss.__r8);
  thread_context.r9 = u64(mcontext->__ss.__r9);
  thread_context.r10 = u64(mcontext->__ss.__r10);
  thread_context.r11 = u64(mcontext->__ss.__r11);
  thread_context.r12 = u64(mcontext->__ss.__r12);
  thread_context.r13 = u64(mcontext->__ss.__r13);
  thread_context.r14 = u64(mcontext->__ss.__r14);
  thread_context.r15 = u64(mcontext->__ss.__r15);
  std::memcpy(thread_context.xmm_registers, &mcontext->__fs.__fpu_xmm0,
              sizeof(thread_context.xmm_registers));
#else
  thread_context.rip = u64(mcontext.gregs[REG_RIP]);
  thread_context.eflags = u32(mcontext.gregs[REG_EFL]);
  // The REG_ order differs from the register numbers in the instruction encoding.
  thread_context.rax = u64(mcontext.gregs[REG_RAX]);
  thread_context.rcx = u64(mcontext.gregs[REG_RCX]);
  thread_context.rdx = u64(mcontext.gregs[REG_RDX]);
  thread_context.rbx = u64(mcontext.gregs[REG_RBX]);
  thread_context.rsp = u64(mcontext.gregs[REG_RSP]);
  thread_context.rbp = u64(mcontext.gregs[REG_RBP]);
  thread_context.rsi = u64(mcontext.gregs[REG_RSI]);
  thread_context.rdi = u64(mcontext.gregs[REG_RDI]);
  thread_context.r8 = u64(mcontext.gregs[REG_R8]);
  thread_context.r9 = u64(mcontext.gregs[REG_R9]);
  thread_context.r10 = u64(mcontext.gregs[REG_R10]);
  thread_context.r11 = u64(mcontext.gregs[REG_R11]);
  thread_context.r12 = u64(mcontext.gregs[REG_R12]);
  thread_context.r13 = u64(mcontext.gregs[REG_R13]);
  thread_context.r14 = u64(mcontext.gregs[REG_R14]);
  thread_context.r15 = u64(mcontext.gregs[REG_R15]);
  std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm,
              sizeof(thread_context.xmm_registers));
#endif
#elif REX_ARCH_ARM64
#if REX_PLATFORM_MAC
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
           reinterpret_cast<u8*>(mcontext_extension) + mcontext_extension->size)) {
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
      // macOS raises SIGBUS for a write to a protected page.
#if !REX_PLATFORM_MAC
      // Elsewhere SIGBUS is a misaligned access, not a protection fault.
      assert_unhandled_case(signal_number);
      return;
#endif
      [[fallthrough]];
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation;
#if REX_ARCH_AMD64
      // X86_PF_WRITE bit of the page fault error code.
      constexpr u64 kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
#if REX_PLATFORM_MAC
      access_violation_operation = (u64(mcontext->__es.__err) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#else
      access_violation_operation = (u64(mcontext.gregs[REG_ERR]) & kX86PageFaultErrorCodeWrite)
                                       ? Exception::AccessViolationOperation::kWrite
                                       : Exception::AccessViolationOperation::kRead;
#endif
#elif REX_ARCH_ARM64
      // Bit 6 of a Data Abort syndrome is set for a write.
#if REX_PLATFORM_MAC
      {
        u32 mac_esr = mcontext->__es.__esr;
        if (((mac_esr >> 26) & 0b111110) == 0b100100) {
          access_violation_operation = (mac_esr & (1U << 6))
                                           ? Exception::AccessViolationOperation::kWrite
                                           : Exception::AccessViolationOperation::kRead;
        } else {
          bool instruction_is_store;
          if (IsArm64LoadPrefetchStore(*reinterpret_cast<const u32*>(thread_context.pc),
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
        // Without esr_context the faulting instruction gives the access direction.
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(*reinterpret_cast<const u32*>(mcontext.pc),
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
      ex.InitializeAccessViolation(&thread_context, reinterpret_cast<u64>(signal_info->si_addr),
                                   access_violation_operation);
    } break;
    default:
      assert_unhandled_case(signal_number);
  }

  for (size_t i = 0; i < rex::countof(g_handlers) && g_handlers[i].first; ++i) {
    if (g_handlers[i].first(&ex, g_handlers[i].second)) {
#if REX_ARCH_AMD64
#if REX_PLATFORM_MAC
      mcontext->__ss.__rip = thread_context.rip;
      mcontext->__ss.__rflags = thread_context.eflags;
      u32 modified_register_index;
      u16 modified_int_registers_remaining = ex.modified_int_registers();
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
      u16 modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(reinterpret_cast<char*>(&mcontext->__fs.__fpu_xmm0) +
                        modified_register_index * sizeof(vec128_t),
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#else
      mcontext.gregs[REG_RIP] = greg_t(thread_context.rip);
      mcontext.gregs[REG_EFL] = greg_t(thread_context.eflags);
      u32 modified_register_index;
      // The order must match the order in X64Register.
      static const size_t kIntRegisterMap[] = {
          REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
          REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
      };
      u16 modified_int_registers_remaining = ex.modified_int_registers();
      while (rex::bit_scan_forward(modified_int_registers_remaining, &modified_register_index)) {
        modified_int_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        mcontext.gregs[kIntRegisterMap[modified_register_index]] =
            thread_context.int_registers[modified_register_index];
      }
      u16 modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (rex::bit_scan_forward(modified_xmm_registers_remaining, &modified_register_index)) {
        modified_xmm_registers_remaining &= ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&mcontext.fpregs->_xmm[modified_register_index],
                    &thread_context.xmm_registers[modified_register_index], sizeof(vec128_t));
      }
#endif
#elif REX_ARCH_ARM64
      u32 modified_register_index;
      u32 modified_x_registers_remaining = ex.modified_x_registers();
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
        u32 modified_v_registers_remaining = ex.modified_v_registers();
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
        u32 modified_v_registers_remaining = ex.modified_v_registers();
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
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!g_signal_handlers_installed) {
    struct sigaction signal_handler;

    std::memset(&signal_handler, 0, sizeof(signal_handler));
    signal_handler.sa_sigaction = ExceptionHandlerCallback;
    signal_handler.sa_flags = SA_SIGINFO;

    if (sigaction(SIGILL, &signal_handler, &g_original_sigill_handler) != 0) {
      assert_always("Failed to install new SIGILL handler");
    }
    if (sigaction(SIGSEGV, &signal_handler, &g_original_sigsegv_handler) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
#if REX_PLATFORM_MAC
    if (sigaction(SIGBUS, &signal_handler, &g_original_sigbus_handler) != 0) {
      assert_always("Failed to install new SIGBUS handler");
    }
#endif
    g_signal_handlers_installed = true;
  }

  for (size_t i = 0; i < rex::countof(g_handlers); ++i) {
    if (!g_handlers[i].first) {
      g_handlers[i].first = fn;
      g_handlers[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < rex::countof(g_handlers); ++i) {
    if (g_handlers[i].first == fn && g_handlers[i].second == data) {
      for (; i < rex::countof(g_handlers) - 1; ++i) {
        g_handlers[i] = g_handlers[i + 1];
      }
      g_handlers[i].first = nullptr;
      g_handlers[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < rex::countof(g_handlers); ++i) {
    if (g_handlers[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (g_signal_handlers_installed) {
      if (sigaction(SIGILL, &g_original_sigill_handler, NULL) != 0) {
        assert_always("Failed to restore original SIGILL handler");
      }
      if (sigaction(SIGSEGV, &g_original_sigsegv_handler, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
#if REX_PLATFORM_MAC
      if (sigaction(SIGBUS, &g_original_sigbus_handler, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
#endif
      g_signal_handlers_installed = false;
    }
  }
}

}  // namespace rex::platform

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
