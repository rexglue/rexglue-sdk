/**
 * @file        platform/exception_handler.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Derived from Xenia's xenia/base/exception_handler.h (Ben Vanik, 2022).
 */

#pragma once

#include <functional>
#include <vector>

#include <rex/assert.h>
#include <rex/platform.h>
#include <rex/types.h>

#if !REX_PLATFORM_WIN32
#include <signal.h>
#endif

namespace rex::platform {

struct alignas(16) VectorRegister {
  u8 bytes[16];
};

/** ARM64 registers, indexed as in the instruction encoding. */
enum class ARM64Register : u8 {
  kX0,
  kX1,
  kX2,
  kX3,
  kX4,
  kX5,
  kX6,
  kX7,
  kX8,
  kX9,
  kX10,
  kX11,
  kX12,
  kX13,
  kX14,
  kX15,
  kX16,
  kX17,
  kX18,
  kX19,
  kX20,
  kX21,
  kX22,
  kX23,
  kX24,
  kX25,
  kX26,
  kX27,
  kX28,
  kX29, /**< Frame pointer. */
  kX30, /**< Link register. */
  kSP,
  kPC,
  kPstate,
  kFpsr,
  kFpcr,
  kV0, /**< All 128 bits, named Q0. */
  kV1,
  kV2,
  kV3,
  kV4,
  kV5,
  kV6,
  kV7,
  kV8,
  kV9,
  kV10,
  kV11,
  kV12,
  kV13,
  kV14,
  kV15,
  kV16,
  kV17,
  kV18,
  kV19,
  kV20,
  kV21,
  kV22,
  kV23,
  kV24,
  kV25,
  kV26,
  kV27,
  kV28,
  kV29,
  kV30,
  kV31,
};

#if REX_ARCH_AMD64

/** x86-64 registers. The general purpose order matches the instruction encoding. */
enum class X64Register : u8 {
  kRip,
  kEflags,
  kIntRegisterFirst,
  kRax = kIntRegisterFirst,
  kRcx,
  kRdx,
  kRbx,
  kRsp,
  kRbp,
  kRsi,
  kRdi,
  kR8,
  kR9,
  kR10,
  kR11,
  kR12,
  kR13,
  kR14,
  kR15,
  kIntRegisterLast = kR15,
  kXMM0,
  kXMM1,
  kXMM2,
  kXMM3,
  kXMM4,
  kXMM5,
  kXMM6,
  kXMM7,
  kXMM8,
  kXMM9,
  kXMM10,
  kXMM11,
  kXMM12,
  kXMM13,
  kXMM14,
  kXMM15,
};

#endif  // REX_ARCH_AMD64

#if REX_ARCH_AMD64
using HostRegister = X64Register;
#elif REX_ARCH_ARM64
using HostRegister = ARM64Register;
#else
/** Register enumeration of the host architecture, empty on other architectures. */
enum class HostRegister {};
#endif  // REX_ARCH

/** Register state of a host thread captured when an exception is raised. */
class HostThreadContext {
 public:
#if REX_ARCH_AMD64
  u64 rip;
  u32 eflags;
  union {
    struct {
      u64 rax;
      u64 rcx;
      u64 rdx;
      u64 rbx;
      u64 rsp;
      u64 rbp;
      u64 rsi;
      u64 rdi;
      u64 r8;
      u64 r9;
      u64 r10;
      u64 r11;
      u64 r12;
      u64 r13;
      u64 r14;
      u64 r15;
    };
    u64 int_registers[16]; /**< In X64Register order. */
  };
  union {
    struct {
      VectorRegister xmm0;
      VectorRegister xmm1;
      VectorRegister xmm2;
      VectorRegister xmm3;
      VectorRegister xmm4;
      VectorRegister xmm5;
      VectorRegister xmm6;
      VectorRegister xmm7;
      VectorRegister xmm8;
      VectorRegister xmm9;
      VectorRegister xmm10;
      VectorRegister xmm11;
      VectorRegister xmm12;
      VectorRegister xmm13;
      VectorRegister xmm14;
      VectorRegister xmm15;
    };
    VectorRegister xmm_registers[16];
  };
#elif REX_ARCH_ARM64
  u64 x[31]; /**< X29 is FP and X30 is LR. */
  u64 sp;
  u64 pc;
  u64 pstate;
  u32 fpsr;
  u32 fpcr;
  VectorRegister v[32];
#endif  // REX_ARCH
};

/*
 * AArch64 load and store decoding based on VIXL.
 * https://github.com/Linaro/vixl/blob/ae5957cd66517b3f31dbf37e9bf39db6594abfe3/src/aarch64/constants-aarch64.h
 *
 * Copyright 2015, VIXL authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *   * Neither the name of ARM Limited nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** Mask for PC relative literal loads, which include PRFM_lit. */
constexpr u32 kARM64LoadLiteralFMask = UINT32_C(0x3B000000);
constexpr u32 kARM64LoadLiteralFixed = UINT32_C(0x18000000);

constexpr u32 kARM64LoadStoreAnyFMask = UINT32_C(0x0A000000);
constexpr u32 kARM64LoadStoreAnyFixed = UINT32_C(0x08000000);

constexpr u32 kARM64LoadStorePairAnyFMask = UINT32_C(0x3A000000);
constexpr u32 kARM64LoadStorePairAnyFixed = UINT32_C(0x28000000);
/** Bit set on the load form of a load or store pair. */
constexpr u32 kARM64LoadStorePairLoadBit = UINT32_C(1) << 22;

/** Selects the ARM64LoadStoreOp bits of a load or store. */
constexpr u32 kARM64LoadStoreMask = UINT32_C(0xC4C00000);

/** Single register load and store operations under kARM64LoadStoreMask. */
enum class ARM64LoadStoreOp : u32 {
  kSTRBW = UINT32_C(0x00000000),  /**< Store byte from W. */
  kSTRHW = UINT32_C(0x40000000),  /**< Store halfword from W. */
  kSTRW = UINT32_C(0x80000000),   /**< Store word from W. */
  kSTRX = UINT32_C(0xC0000000),   /**< Store doubleword from X. */
  kLDRBW = UINT32_C(0x00400000),  /**< Load byte into W. */
  kLDRHW = UINT32_C(0x40400000),  /**< Load halfword into W. */
  kLDRW = UINT32_C(0x80400000),   /**< Load word into W. */
  kLDRX = UINT32_C(0xC0400000),   /**< Load doubleword into X. */
  kLDRSBX = UINT32_C(0x00800000), /**< Load signed byte into X. */
  kLDRSHX = UINT32_C(0x40800000), /**< Load signed halfword into X. */
  kLDRSWX = UINT32_C(0x80800000), /**< Load signed word into X. */
  kLDRSBW = UINT32_C(0x00C00000), /**< Load signed byte into W. */
  kLDRSHW = UINT32_C(0x40C00000), /**< Load signed halfword into W. */
  kSTRB = UINT32_C(0x04000000),   /**< Store 8 bit SIMD register. */
  kSTRH = UINT32_C(0x44000000),   /**< Store 16 bit SIMD register. */
  kSTRS = UINT32_C(0x84000000),   /**< Store 32 bit SIMD register. */
  kSTRD = UINT32_C(0xC4000000),   /**< Store 64 bit SIMD register. */
  kSTRQ = UINT32_C(0x04800000),   /**< Store 128 bit SIMD register. */
  kLDRB = UINT32_C(0x04400000),   /**< Load 8 bit SIMD register. */
  kLDRH = UINT32_C(0x44400000),   /**< Load 16 bit SIMD register. */
  kLDRS = UINT32_C(0x84400000),   /**< Load 32 bit SIMD register. */
  kLDRD = UINT32_C(0xC4400000),   /**< Load 64 bit SIMD register. */
  kLDRQ = UINT32_C(0x04C00000),   /**< Load 128 bit SIMD register. */
  kPRFM = UINT32_C(0xC0800000),   /**< Prefetch. */
};

/** Mask for the load and store offset addressing modes. */
constexpr u32 kARM64LoadStoreOffsetFMask = UINT32_C(0x3B200C00);

/** Fixed bits of each load and store offset addressing mode. */
enum class ARM64LoadStoreOffsetFixed : u32 {
  kUnscaledOffset = UINT32_C(0x38000000), /**< Unscaled immediate offset. */
  kPostIndex = UINT32_C(0x38000400),      /**< Post indexed immediate. */
  kPreIndex = UINT32_C(0x38000C00),       /**< Pre indexed immediate. */
  kRegisterOffset = UINT32_C(0x38200800),
};

/** Mask for the unsigned immediate offset addressing mode. */
constexpr u32 kARM64LoadStoreUnsignedOffsetFMask = UINT32_C(0x3B000000);
constexpr u32 kARM64LoadStoreUnsignedOffsetFixed = UINT32_C(0x39000000);

/**
 * Classifies an ARM64 instruction as a load, prefetch or store.
 *
 * @param instruction The instruction word.
 * @param is_store_out True for a store. Unchanged for a PC relative literal load or a false return.
 * @return True when the instruction is a load, prefetch or store.
 */
REX_API bool IsARM64LoadPrefetchStore(u32 instruction, bool& is_store_out);

/** Host exception passed to each ExceptionHandler::Handler. */
class Exception {
 public:
  enum class Code : u8 {
    kInvalidException = 0, /**< Not initialized. */
    kAccessViolation,
    kIllegalInstruction,
  };

  enum class AccessViolationOperation : u8 {
    kUnknown,
    kRead,
    kWrite,
  };

  /**
   * Initializes this exception as an access violation.
   *
   * @param fault_address The address read or written.
   */
  void InitializeAccessViolation(HostThreadContext* thread_context, u64 fault_address,
                                 AccessViolationOperation operation) {
    code_ = Code::kAccessViolation;
    thread_context_ = thread_context;
    fault_address_ = fault_address;
    access_violation_operation_ = operation;
  }

  void InitializeIllegalInstruction(HostThreadContext* thread_context) {
    code_ = Code::kIllegalInstruction;
    thread_context_ = thread_context;
  }

  Code GetCode() const { return code_; }

  /**
   * Returns the captured thread context. Write registers through the Modify functions so the change
   * reaches the thread when it resumes.
   */
  HostThreadContext* GetThreadContext() const { return thread_context_; }

  u64 GetPC() const {
#if REX_ARCH_AMD64
    return thread_context_->rip;
#elif REX_ARCH_ARM64
    return thread_context_->pc;
#else
    REX_ASSERT_ALWAYS();
    return 0;
#endif  // REX_ARCH
  }

  void SetResumePC(u64 pc) {
#if REX_ARCH_AMD64
    thread_context_->rip = pc;
#elif REX_ARCH_ARM64
    thread_context_->pc = pc;
#else
    REX_ASSERT_ALWAYS();
#endif  // REX_ARCH
  }

#if REX_ARCH_AMD64
  /**
   * Marks a general purpose register modified and returns it for writing.
   *
   * @param index The index relative to X64Register::kIntRegisterFirst, 0 to 15.
   */
  u64& ModifyIntRegister(u32 index) {
    REX_ASSERT(index <= 15);
    modified_int_registers_ |= UINT16_C(1) << index;
    return thread_context_->int_registers[index];
  }

  /** Returns the modified general purpose registers. The mask has one bit per register index. */
  u16 GetModifiedIntRegisters() const { return modified_int_registers_; }

  /**
   * Marks an XMM register modified and returns it for writing.
   *
   * @param index The XMM register index, 0 to 15.
   */
  VectorRegister& ModifyXMMRegister(u32 index) {
    REX_ASSERT(index <= 15);
    modified_xmm_registers_ |= UINT16_C(1) << index;
    return thread_context_->xmm_registers[index];
  }

  /**
   * @return Bit mask by register index.
   */
  u16 GetModifiedXMMRegisters() const { return modified_xmm_registers_; }
#elif REX_ARCH_ARM64
  /**
   * Marks an X register modified and returns it for writing.
   *
   * @param index The X register index, 0 to 30.
   */
  u64& ModifyXRegister(u32 index) {
    REX_ASSERT(index <= 30);
    modified_x_registers_ |= UINT32_C(1) << index;
    return thread_context_->x[index];
  }

  /**
   * @return Bit mask by register index.
   */
  u32 GetModifiedXRegisters() const { return modified_x_registers_; }

  /**
   * Marks a V register modified and returns it for writing.
   *
   * @param index The V register index, 0 to 31.
   */
  VectorRegister& ModifyVRegister(u32 index) {
    REX_ASSERT(index <= 31);
    modified_v_registers_ |= UINT32_C(1) << index;
    return thread_context_->v[index];
  }

  /**
   * @return Bit mask by register index.
   */
  u32 GetModifiedVRegisters() const { return modified_v_registers_; }
#endif  // REX_ARCH

  /** Returns the fault address of an access violation. It is the address read or written. */
  u64 GetFaultAddress() const { return fault_address_; }

  AccessViolationOperation GetAccessViolationOperation() const {
    return access_violation_operation_;
  }

 private:
  Code code_ = Code::kInvalidException;
  HostThreadContext* thread_context_ = nullptr;
#if REX_ARCH_AMD64
  u16 modified_int_registers_ = 0;
  u16 modified_xmm_registers_ = 0;
#elif REX_ARCH_ARM64
  u32 modified_x_registers_ = 0;
  u32 modified_v_registers_ = 0;
#endif  // REX_ARCH
  u64 fault_address_ = 0;
  AccessViolationOperation access_violation_operation_ = AccessViolationOperation::kUnknown;
};

/** Process wide chain of host exception handlers. */
class REX_API ExceptionHandler {
 public:
  /**
   * Exception callback. Returns true when the exception is handled, which resumes the thread with
   * the modified context, or false to pass it to the next handler.
   */
  using Handler = bool (*)(Exception* ex, void* data);

  /** Adds a handler to the end of the chain. */
  static void Install(Handler fn, void* data);

  /**
   * Removes a handler added with the same fn and data.
   *
   * @param data The value it was installed with.
   */
  static void Uninstall(Handler fn, void* data);
};

#if !REX_PLATFORM_WIN32
/**
 * Returns the instruction pointer a signal interrupted.
 *
 * @param signal_context The ucontext_t the signal handler receives.
 */
REX_API u64 SignalPC(void* signal_context);

/**
 * Writes a report for a fatal signal when the reporter is installed and no report has run.
 *
 * @param info The signal info, or null.
 * @param signal_context The ucontext of the fault, or null.
 */
REX_API void ReportFatalSignal(int signal_number, siginfo_t* info, void* signal_context);

/**
 * Passes a signal no SDK handler claimed to the action it replaced. A default or ignored action, or
 * the same fault 64 times in a row, ends in a report and the default disposition.
 *
 * @param original The replaced action. Cleared to the default when it carries SA_RESETHAND.
 */
REX_API void ForwardSignal(struct sigaction& original, int signal_number, siginfo_t* info,
                           void* signal_context);

/**
 * Ends the process through the default disposition of a signal.
 */
[[noreturn]] REX_API void DieWithDefaultDisposition(int signal_number);
#endif

}  // namespace rex::platform
