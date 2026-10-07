/**
 * @file        platform/fpscr.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

#include <rex/types.h>

#include <simde/x86/sse.h>

#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif

namespace rex::platform {

#if defined(__x86_64__) || defined(_M_X64)

/** Host floating point control register access for x86-64 MXCSR. */
struct FPSCRPlatform {
  static constexpr size_t RoundShift = 13;                    /**< MXCSR RC field shift. */
  static constexpr size_t RoundMaskVal = SIMDE_MM_ROUND_MASK; /**< MXCSR RC field mask. */
  /** MXCSR FTZ and DAZ bits. */
  static constexpr size_t FlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  /** MXCSR IM, DM, ZM, OM, UM and PM bits. A set bit masks the exception. */
  static constexpr u32 ExceptionMask =
      (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12);
  /** MXCSR rounding bits indexed by the PowerPC FPSCR RN field. */
  static constexpr size_t GuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                           SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};

  /**
   * Reads MXCSR.
   *
   * @return Current MXCSR value.
   */
  static inline u32 getcsr() noexcept { return simde_mm_getcsr(); }

  /**
   * Writes MXCSR.
   *
   * @param csr New MXCSR value.
   */
  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }

  /**
   * Masks every floating point exception in a control word.
   *
   * @param csr Control word to update.
   */
  static inline void InitHostExceptions(u32& csr) noexcept { csr |= ExceptionMask; }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

/** Host floating point control register access for ARM64 FPCR. */
struct FPSCRPlatform {
  static constexpr size_t RoundShift = 22;                   /**< FPCR RMode field shift. */
  static constexpr size_t RoundMaskVal = 3 << RoundShift;    /**< FPCR RMode field mask. */
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24); /**< FPCR FZ16 and FZ bits. */
  /** FPCR rounding bits indexed by the PowerPC FPSCR RN field. */
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  /** FPCR IOE, DZE, OFE, UFE, IXE and IDE bits. A clear bit disables the trap. */
  static constexpr u32 ExceptionMask =
      (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 15);

  /**
   * Reads FPCR.
   *
   * @return Current FPCR value.
   */
  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }

  /**
   * Writes FPCR.
   *
   * @param csr New FPCR value.
   */
  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(csr)); }

  /**
   * Disables every floating point trap in a control word.
   *
   * @param csr Control word to update.
   */
  static inline void InitHostExceptions(u32& csr) noexcept { csr &= ~ExceptionMask; }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::platform
