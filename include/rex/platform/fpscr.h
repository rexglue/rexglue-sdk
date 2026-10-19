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
  static constexpr size_t kRoundShift = 13;                    /**< MXCSR RC field shift. */
  static constexpr size_t kRoundMaskVal = SIMDE_MM_ROUND_MASK; /**< MXCSR RC field mask. */
  /** MXCSR FTZ and DAZ bits. */
  static constexpr size_t kFlushMask = SIMDE_MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
  /** MXCSR IM, DM, ZM, OM, UM and PM bits. A set bit masks the exception. */
  static constexpr u32 kExceptionMask =
      (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12);
  /** MXCSR rounding bits indexed by the PowerPC FPSCR RN field. */
  static constexpr size_t kGuestToHost[] = {SIMDE_MM_ROUND_NEAREST, SIMDE_MM_ROUND_TOWARD_ZERO,
                                            SIMDE_MM_ROUND_UP, SIMDE_MM_ROUND_DOWN};
  /** PowerPC FPSCR RN values indexed by the MXCSR RC field. */
  static constexpr u32 kHostToGuest[] = {0, 3, 2, 1};

  /**
   * Reads MXCSR.
   *
   * @return Current MXCSR value.
   */
  static u32 GetCSR() noexcept { return simde_mm_getcsr(); }

  /**
   * Writes MXCSR.
   *
   * @param csr New MXCSR value.
   */
  static void SetCSR(u32 csr) noexcept { simde_mm_setcsr(csr); }

  /**
   * Masks every floating point exception in a control word.
   *
   * @param csr Control word to update.
   */
  static void InitHostExceptions(u32& csr) noexcept { csr |= kExceptionMask; }
};

#elif defined(__aarch64__) || defined(_M_ARM64)

/** Host floating point control register access for ARM64 FPCR. */
struct FPSCRPlatform {
  static constexpr size_t kRoundShift = 22;                 /**< FPCR RMode field shift. */
  static constexpr size_t kRoundMaskVal = 3 << kRoundShift; /**< FPCR RMode field mask. */
  static constexpr size_t kFlushMask = 1 << 24;             /**< FPCR FZ bit. */
  /** FPCR rounding bits indexed by the PowerPC FPSCR RN field. */
  static constexpr size_t kGuestToHost[] = {0 << kRoundShift, 3 << kRoundShift, 1 << kRoundShift,
                                            2 << kRoundShift};
  /** PowerPC FPSCR RN values indexed by the FPCR RMode field. */
  static constexpr u32 kHostToGuest[] = {0, 2, 3, 1};
  /** FPCR IOE, DZE, OFE, UFE, IXE and IDE bits. A clear bit disables the trap. */
  static constexpr u32 kExceptionMask =
      (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 15);

  /**
   * Reads FPCR.
   *
   * @return Current FPCR value.
   */
  static inline u32 GetCSR() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return static_cast<u32>(csr);
  }

  /**
   * Writes FPCR.
   *
   * @param csr New FPCR value.
   */
  static inline void SetCSR(u32 csr) noexcept {
    __asm__ __volatile__("msr fpcr, %0" : : "r"(static_cast<u64>(csr)));
  }

  /**
   * Disables every floating point trap in a control word.
   *
   * @param csr Control word to update.
   */
  static inline void InitHostExceptions(u32& csr) noexcept { csr &= ~kExceptionMask; }
};

#else
#error "Missing implementation for FPSCR."
#endif

}  // namespace rex::platform
