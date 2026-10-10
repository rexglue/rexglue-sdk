/**
 * @file        exception_handler.h
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
#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <rex/vec128.h>

namespace rex::platform {

/**
 * ARM64 registers. The X and V indices match the register numbers in the instruction encoding and
 * the remapping tables in the exception handler implementations.
 */
enum class Arm64Register : u8 {
  kX0,     /**< General purpose register X0. */
  kX1,     /**< General purpose register X1. */
  kX2,     /**< General purpose register X2. */
  kX3,     /**< General purpose register X3. */
  kX4,     /**< General purpose register X4. */
  kX5,     /**< General purpose register X5. */
  kX6,     /**< General purpose register X6. */
  kX7,     /**< General purpose register X7. */
  kX8,     /**< General purpose register X8. */
  kX9,     /**< General purpose register X9. */
  kX10,    /**< General purpose register X10. */
  kX11,    /**< General purpose register X11. */
  kX12,    /**< General purpose register X12. */
  kX13,    /**< General purpose register X13. */
  kX14,    /**< General purpose register X14. */
  kX15,    /**< General purpose register X15. */
  kX16,    /**< General purpose register X16. */
  kX17,    /**< General purpose register X17. */
  kX18,    /**< General purpose register X18. */
  kX19,    /**< General purpose register X19. */
  kX20,    /**< General purpose register X20. */
  kX21,    /**< General purpose register X21. */
  kX22,    /**< General purpose register X22. */
  kX23,    /**< General purpose register X23. */
  kX24,    /**< General purpose register X24. */
  kX25,    /**< General purpose register X25. */
  kX26,    /**< General purpose register X26. */
  kX27,    /**< General purpose register X27. */
  kX28,    /**< General purpose register X28. */
  kX29,    /**< Frame pointer X29. */
  kX30,    /**< Link register X30. */
  kSp,     /**< Stack pointer. */
  kPc,     /**< Program counter. */
  kPstate, /**< Processor state. */
  kFpsr,   /**< Floating point status register. */
  kFpcr,   /**< Floating point control register. */
  kV0,     /**< SIMD register V0, all 128 bits, also named Q0. */
  kV1,     /**< SIMD register V1, all 128 bits, also named Q1. */
  kV2,     /**< SIMD register V2, all 128 bits, also named Q2. */
  kV3,     /**< SIMD register V3, all 128 bits, also named Q3. */
  kV4,     /**< SIMD register V4, all 128 bits, also named Q4. */
  kV5,     /**< SIMD register V5, all 128 bits, also named Q5. */
  kV6,     /**< SIMD register V6, all 128 bits, also named Q6. */
  kV7,     /**< SIMD register V7, all 128 bits, also named Q7. */
  kV8,     /**< SIMD register V8, all 128 bits, also named Q8. */
  kV9,     /**< SIMD register V9, all 128 bits, also named Q9. */
  kV10,    /**< SIMD register V10, all 128 bits, also named Q10. */
  kV11,    /**< SIMD register V11, all 128 bits, also named Q11. */
  kV12,    /**< SIMD register V12, all 128 bits, also named Q12. */
  kV13,    /**< SIMD register V13, all 128 bits, also named Q13. */
  kV14,    /**< SIMD register V14, all 128 bits, also named Q14. */
  kV15,    /**< SIMD register V15, all 128 bits, also named Q15. */
  kV16,    /**< SIMD register V16, all 128 bits, also named Q16. */
  kV17,    /**< SIMD register V17, all 128 bits, also named Q17. */
  kV18,    /**< SIMD register V18, all 128 bits, also named Q18. */
  kV19,    /**< SIMD register V19, all 128 bits, also named Q19. */
  kV20,    /**< SIMD register V20, all 128 bits, also named Q20. */
  kV21,    /**< SIMD register V21, all 128 bits, also named Q21. */
  kV22,    /**< SIMD register V22, all 128 bits, also named Q22. */
  kV23,    /**< SIMD register V23, all 128 bits, also named Q23. */
  kV24,    /**< SIMD register V24, all 128 bits, also named Q24. */
  kV25,    /**< SIMD register V25, all 128 bits, also named Q25. */
  kV26,    /**< SIMD register V26, all 128 bits, also named Q26. */
  kV27,    /**< SIMD register V27, all 128 bits, also named Q27. */
  kV28,    /**< SIMD register V28, all 128 bits, also named Q28. */
  kV29,    /**< SIMD register V29, all 128 bits, also named Q29. */
  kV30,    /**< SIMD register V30, all 128 bits, also named Q30. */
  kV31,    /**< SIMD register V31, all 128 bits, also named Q31. */
};

#if REX_ARCH_AMD64

/**
 * x86-64 registers. The general purpose order matches the instruction encoding, the Windows
 * CONTEXT structure and the remapping tables in the exception handler implementations.
 */
enum class X64Register : u8 {
  kRip,                     /**< Instruction pointer. */
  kEflags,                  /**< Flags register. */
  kIntRegisterFirst,        /**< First general purpose register. */
  kRax = kIntRegisterFirst, /**< RAX. */
  kRcx,                     /**< RCX. */
  kRdx,                     /**< RDX. */
  kRbx,                     /**< RBX. */
  kRsp,                     /**< RSP. */
  kRbp,                     /**< RBP. */
  kRsi,                     /**< RSI. */
  kRdi,                     /**< RDI. */
  kR8,                      /**< R8. */
  kR9,                      /**< R9. */
  kR10,                     /**< R10. */
  kR11,                     /**< R11. */
  kR12,                     /**< R12. */
  kR13,                     /**< R13. */
  kR14,                     /**< R14. */
  kR15,                     /**< R15. */
  kIntRegisterLast = kR15,  /**< Last general purpose register. */
  kXmm0,                    /**< XMM0. */
  kXmm1,                    /**< XMM1. */
  kXmm2,                    /**< XMM2. */
  kXmm3,                    /**< XMM3. */
  kXmm4,                    /**< XMM4. */
  kXmm5,                    /**< XMM5. */
  kXmm6,                    /**< XMM6. */
  kXmm7,                    /**< XMM7. */
  kXmm8,                    /**< XMM8. */
  kXmm9,                    /**< XMM9. */
  kXmm10,                   /**< XMM10. */
  kXmm11,                   /**< XMM11. */
  kXmm12,                   /**< XMM12. */
  kXmm13,                   /**< XMM13. */
  kXmm14,                   /**< XMM14. */
  kXmm15,                   /**< XMM15. */
};

#endif  // REX_ARCH_AMD64

#if REX_ARCH_AMD64
/** Register enumeration of the host architecture. */
using HostRegister = X64Register;
#elif REX_ARCH_ARM64
/** Register enumeration of the host architecture. */
using HostRegister = Arm64Register;
#else
/** Register enumeration of the host architecture, empty on other architectures. */
enum class HostRegister {};
#endif  // REX_ARCH

/**
 * Register state of a host thread captured when an exception is raised.
 */
class HostThreadContext {
 public:
#if REX_ARCH_AMD64
  u64 rip;    /**< Instruction pointer. */
  u32 eflags; /**< Flags register. */
  union {
    struct {
      u64 rax; /**< RAX. */
      u64 rcx; /**< RCX. */
      u64 rdx; /**< RDX. */
      u64 rbx; /**< RBX. */
      u64 rsp; /**< RSP. */
      u64 rbp; /**< RBP. */
      u64 rsi; /**< RSI. */
      u64 rdi; /**< RDI. */
      u64 r8;  /**< R8. */
      u64 r9;  /**< R9. */
      u64 r10; /**< R10. */
      u64 r11; /**< R11. */
      u64 r12; /**< R12. */
      u64 r13; /**< R13. */
      u64 r14; /**< R14. */
      u64 r15; /**< R15. */
    };
    u64 int_registers[16]; /**< General purpose registers in X64Register order. */
  };
  union {
    struct {
      vec128_t xmm0;  /**< XMM0. */
      vec128_t xmm1;  /**< XMM1. */
      vec128_t xmm2;  /**< XMM2. */
      vec128_t xmm3;  /**< XMM3. */
      vec128_t xmm4;  /**< XMM4. */
      vec128_t xmm5;  /**< XMM5. */
      vec128_t xmm6;  /**< XMM6. */
      vec128_t xmm7;  /**< XMM7. */
      vec128_t xmm8;  /**< XMM8. */
      vec128_t xmm9;  /**< XMM9. */
      vec128_t xmm10; /**< XMM10. */
      vec128_t xmm11; /**< XMM11. */
      vec128_t xmm12; /**< XMM12. */
      vec128_t xmm13; /**< XMM13. */
      vec128_t xmm14; /**< XMM14. */
      vec128_t xmm15; /**< XMM15. */
    };
    vec128_t xmm_registers[16]; /**< XMM0 to XMM15. */
  };
#elif REX_ARCH_ARM64
  u64 x[31];      /**< X0 to X30, with X29 as FP and X30 as LR. */
  u64 sp;         /**< Stack pointer. */
  u64 pc;         /**< Program counter. */
  u64 pstate;     /**< Processor state. */
  u32 fpsr;       /**< Floating point status register. */
  u32 fpcr;       /**< Floating point control register. */
  vec128_t v[32]; /**< V0 to V31. */
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
constexpr u32 kArm64LoadLiteralFMask = UINT32_C(0x3B000000);
/** Fixed bits of PC relative literal loads under kArm64LoadLiteralFMask. */
constexpr u32 kArm64LoadLiteralFixed = UINT32_C(0x18000000);

/** Mask for any load or store. */
constexpr u32 kArm64LoadStoreAnyFMask = UINT32_C(0x0A000000);
/** Fixed bits of any load or store under kArm64LoadStoreAnyFMask. */
constexpr u32 kArm64LoadStoreAnyFixed = UINT32_C(0x08000000);

/** Mask for any load or store pair. */
constexpr u32 kArm64LoadStorePairAnyFMask = UINT32_C(0x3A000000);
/** Fixed bits of any load or store pair under kArm64LoadStorePairAnyFMask. */
constexpr u32 kArm64LoadStorePairAnyFixed = UINT32_C(0x28000000);
/** Bit set on the load form of a load or store pair. */
constexpr u32 kArm64LoadStorePairLoadBit = UINT32_C(1) << 22;

/** Mask that selects the Arm64LoadStoreOp bits of a load or store. */
constexpr u32 kArm64LoadStoreMask = UINT32_C(0xC4C00000);

/**
 * Single register load and store operations under kArm64LoadStoreMask.
 */
enum class Arm64LoadStoreOp : u32 {
  kSTRB_w = UINT32_C(0x00000000),  /**< Store byte from W. */
  kSTRH_w = UINT32_C(0x40000000),  /**< Store halfword from W. */
  kSTR_w = UINT32_C(0x80000000),   /**< Store word from W. */
  kSTR_x = UINT32_C(0xC0000000),   /**< Store doubleword from X. */
  kLDRB_w = UINT32_C(0x00400000),  /**< Load byte into W. */
  kLDRH_w = UINT32_C(0x40400000),  /**< Load halfword into W. */
  kLDR_w = UINT32_C(0x80400000),   /**< Load word into W. */
  kLDR_x = UINT32_C(0xC0400000),   /**< Load doubleword into X. */
  kLDRSB_x = UINT32_C(0x00800000), /**< Load signed byte into X. */
  kLDRSH_x = UINT32_C(0x40800000), /**< Load signed halfword into X. */
  kLDRSW_x = UINT32_C(0x80800000), /**< Load signed word into X. */
  kLDRSB_w = UINT32_C(0x00C00000), /**< Load signed byte into W. */
  kLDRSH_w = UINT32_C(0x40C00000), /**< Load signed halfword into W. */
  kSTR_b = UINT32_C(0x04000000),   /**< Store 8 bit SIMD register. */
  kSTR_h = UINT32_C(0x44000000),   /**< Store 16 bit SIMD register. */
  kSTR_s = UINT32_C(0x84000000),   /**< Store 32 bit SIMD register. */
  kSTR_d = UINT32_C(0xC4000000),   /**< Store 64 bit SIMD register. */
  kSTR_q = UINT32_C(0x04800000),   /**< Store 128 bit SIMD register. */
  kLDR_b = UINT32_C(0x04400000),   /**< Load 8 bit SIMD register. */
  kLDR_h = UINT32_C(0x44400000),   /**< Load 16 bit SIMD register. */
  kLDR_s = UINT32_C(0x84400000),   /**< Load 32 bit SIMD register. */
  kLDR_d = UINT32_C(0xC4400000),   /**< Load 64 bit SIMD register. */
  kLDR_q = UINT32_C(0x04C00000),   /**< Load 128 bit SIMD register. */
  kPRFM = UINT32_C(0xC0800000),    /**< Prefetch. */
};

/** Mask for the load and store offset addressing modes. */
constexpr u32 kArm64LoadStoreOffsetFMask = UINT32_C(0x3B200C00);

/**
 * Fixed bits of each load and store offset addressing mode under kArm64LoadStoreOffsetFMask.
 */
enum class Arm64LoadStoreOffsetFixed : u32 {
  kUnscaledOffset = UINT32_C(0x38000000), /**< Unscaled immediate offset. */
  kPostIndex = UINT32_C(0x38000400),      /**< Post indexed immediate. */
  kPreIndex = UINT32_C(0x38000C00),       /**< Pre indexed immediate. */
  kRegisterOffset = UINT32_C(0x38200800), /**< Register offset. */
};

/** Mask for the unsigned immediate offset addressing mode. */
constexpr u32 kArm64LoadStoreUnsignedOffsetFMask = UINT32_C(0x3B000000);
/** Fixed bits of the unsigned immediate offset mode under kArm64LoadStoreUnsignedOffsetFMask. */
constexpr u32 kArm64LoadStoreUnsignedOffsetFixed = UINT32_C(0x39000000);

/**
 * Classifies an ARM64 instruction as a load, prefetch or store.
 *
 * @param instruction The instruction word.
 * @param is_store_out Set to true for a store and false for a load or prefetch. It is left
 *        unchanged for a PC relative literal load and when the result is false.
 *
 * @return True when the instruction is a load, prefetch or store.
 */
REX_API bool IsArm64LoadPrefetchStore(u32 instruction, bool& is_store_out);

/**
 * Host exception passed to each ExceptionHandler::Handler.
 */
class Exception {
 public:
  /** Exception kind. */
  enum class Code : u8 {
    kInvalidException = 0, /**< Not initialized. */
    kAccessViolation,      /**< Memory access violation. */
    kIllegalInstruction,   /**< Illegal instruction. */
  };

  /** Memory operation that raised an access violation. */
  enum class AccessViolationOperation : u8 {
    kUnknown, /**< Unknown operation. */
    kRead,    /**< Read. */
    kWrite,   /**< Write. */
  };

  /**
   * Initializes this exception as an access violation.
   *
   * @param thread_context The captured thread context.
   * @param fault_address The address that was read or written.
   * @param operation The operation that faulted.
   */
  void InitializeAccessViolation(HostThreadContext* thread_context, u64 fault_address,
                                 AccessViolationOperation operation) {
    code_ = Code::kAccessViolation;
    thread_context_ = thread_context;
    fault_address_ = fault_address;
    access_violation_operation_ = operation;
  }

  /**
   * Initializes this exception as an illegal instruction.
   *
   * @param thread_context The captured thread context.
   */
  void InitializeIllegalInstruction(HostThreadContext* thread_context) {
    code_ = Code::kIllegalInstruction;
    thread_context_ = thread_context;
  }

  /**
   * @return The exception kind.
   */
  Code code() const { return code_; }

  /**
   * Returns the captured thread context. Write general purpose and vector registers through the
   * Modify functions so the change reaches the thread when it resumes. On x86-64 that covers RAX
   * to R15 and the XMM registers. On ARM64 it covers the X registers, including FP and LR, and the
   * V registers.
   *
   * @return The captured thread context.
   */
  HostThreadContext* thread_context() const { return thread_context_; }

  /**
   * @return The program counter where the exception was raised.
   */
  u64 pc() const {
#if REX_ARCH_AMD64
    return thread_context_->rip;
#elif REX_ARCH_ARM64
    return thread_context_->pc;
#else
    REX_ASSERT_ALWAYS();
    return 0;
#endif  // REX_ARCH
  }

  /**
   * Sets the program counter where execution resumes.
   *
   * @param pc The resume address.
   */
  void set_resume_pc(u64 pc) {
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
   * Marks a general purpose register as modified and returns it for writing.
   *
   * @param index The register index relative to X64Register::kIntRegisterFirst, 0 to 15.
   * @return The register in the thread context.
   */
  u64& ModifyIntRegister(u32 index) {
    REX_ASSERT(index <= 15);
    modified_int_registers_ |= UINT16_C(1) << index;
    return thread_context_->int_registers[index];
  }

  /**
   * @return The bit mask of the general purpose registers marked as modified.
   */
  u16 modified_int_registers() const { return modified_int_registers_; }

  /**
   * Marks an XMM register as modified and returns it for writing.
   *
   * @param index The XMM register index, 0 to 15.
   * @return The register in the thread context.
   */
  vec128_t& ModifyXmmRegister(u32 index) {
    REX_ASSERT(index <= 15);
    modified_xmm_registers_ |= UINT16_C(1) << index;
    return thread_context_->xmm_registers[index];
  }

  /**
   * @return The bit mask of the XMM registers marked as modified.
   */
  u16 modified_xmm_registers() const { return modified_xmm_registers_; }
#elif REX_ARCH_ARM64
  /**
   * Marks an X register as modified and returns it for writing.
   *
   * @param index The X register index, 0 to 30.
   * @return The register in the thread context.
   */
  u64& ModifyXRegister(u32 index) {
    REX_ASSERT(index <= 30);
    modified_x_registers_ |= UINT32_C(1) << index;
    return thread_context_->x[index];
  }

  /**
   * @return The bit mask of the X registers marked as modified.
   */
  u32 modified_x_registers() const { return modified_x_registers_; }

  /**
   * Marks a V register as modified and returns it for writing.
   *
   * @param index The V register index, 0 to 31.
   * @return The register in the thread context.
   */
  vec128_t& ModifyVRegister(u32 index) {
    REX_ASSERT(index <= 31);
    modified_v_registers_ |= UINT32_C(1) << index;
    return thread_context_->v[index];
  }

  /**
   * @return The bit mask of the V registers marked as modified.
   */
  u32 modified_v_registers() const { return modified_v_registers_; }
#endif  // REX_ARCH

  /**
   * @return The address that was read or written, for an access violation.
   */
  u64 fault_address() const { return fault_address_; }

  /**
   * @return The operation that faulted, for an access violation.
   */
  AccessViolationOperation access_violation_operation() const {
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

/**
 * Process-wide chain of host exception handlers.
 */
class REX_API ExceptionHandler {
 public:
  /**
   * Exception callback. It returns true when the exception is handled, which resumes the thread
   * with the modified context, or false to pass the exception to the next handler.
   */
  using Handler = bool (*)(Exception* ex, void* data);

  /**
   * Adds a handler to the end of the chain. Handlers run in install order.
   *
   * @param fn The handler.
   * @param data The value passed to the handler.
   */
  static void Install(Handler fn, void* data);

  /**
   * Removes a handler added with the same fn and data.
   *
   * @param fn The handler.
   * @param data The value it was installed with.
   */
  static void Uninstall(Handler fn, void* data);
};

}  // namespace rex::platform
