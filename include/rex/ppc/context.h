/**
 * @file        ppc/context.h
 * @brief       PPC register types and thread context
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

/*
 * Parts of the PPCContext infrastructure are based on XenonRecomp and UnleashedRecomp.
 */

#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/func.h>
#include <rex/types.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

namespace rex::runtime {
class ThreadState;
}  // namespace rex::runtime

namespace rex::ppc::detail {
struct DetourFrame;
}  // namespace rex::ppc::detail

namespace rex::ppc {

/** General purpose register. */
union Register {
  i8 s8;        /**< Signed 8 bit view of the low byte. */
  uint8_t u8;   /**< Unsigned 8 bit view of the low byte. */
  i16 s16;      /**< Signed 16 bit view of the low half word. */
  uint16_t u16; /**< Unsigned 16 bit view of the low half word. */
  i32 s32;      /**< Signed 32 bit view of the low word. */
  uint32_t u32; /**< Unsigned 32 bit view of the low word. */
  i64 s64;      /**< Signed 64 bit view of the whole register. */
  uint64_t u64; /**< Unsigned 64 bit view of the whole register. */
  float f32;    /**< 32 bit float view of the low word. */
  double f64;   /**< 64 bit float view of the whole register. */
};

/** Fixed point exception register (XER). */
struct XERRegister {
  u8 so; /**< XER bit 0, summary overflow, 1 once an overflow occurred. */
  u8 ov; /**< XER bit 1, overflow of the last arithmetic operation. */
  u8 ca; /**< XER bit 2, carry out of the last arithmetic operation. */
};

/** Condition register (CR) field. */
struct CRRegister {
  u8 lt; /**< CR field bit 0, 1 when the left operand compared less than the right. */
  u8 gt; /**< CR field bit 1, 1 when the left operand compared greater than the right. */
  u8 eq; /**< CR field bit 2, 1 when the operands compared equal. */
  union {
    u8 so; /**< CR field bit 3, copy of the XER so after an integer compare. */
    u8 un; /**< CR field bit 3, 1 when a float compare had a NaN operand. */
  };

  /**
   * Returns the 4 bit CR field value.
   *
   * @return lt in bit 3 down to so in bit 0.
   */
  u32 GetRaw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  /**
   * Sets lt, gt, eq and so from the low 4 bits of a CR field value.
   *
   * @param value The CR field value, lt in bit 3 and so in bit 0.
   */
  void SetRaw(u32 value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  /**
   * Sets lt, gt and eq from an ordered comparison and copies so from the XER.
   *
   * @tparam T The compared type.
   * @param left The left operand.
   * @param right The right operand.
   * @param xer The XER whose so is copied.
   */
  template <typename T>
  void Compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  /**
   * Sets lt, gt and eq from a floating point comparison, and un when either operand is NaN.
   *
   * @param left The left operand.
   * @param right The right operand.
   */
  void Compare(f64 left, f64 right) noexcept {
    un = std::isnan(left) || std::isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
  }

  /**
   * Sets lt when the lane sign mask of a float vector equals imm and eq when no sign bit is set.
   * Clears gt and so.
   *
   * @param mask The vector whose 4 lane sign bits are tested.
   * @param imm The sign mask that sets lt.
   */
  void SetFromMask(simde__m128 mask, int imm) noexcept {
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  /**
   * Sets lt when the byte sign mask of an integer vector equals imm and eq when no sign bit is set.
   * Clears gt and so.
   *
   * @param mask The vector whose 16 byte sign bits are tested.
   * @param imm The sign mask that sets lt.
   */
  void SetFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

/** 128 bit vector register. */
union alignas(0x10) VRegister {
  i8 s8[16];       /**< 16 signed 8 bit lanes. */
  uint8_t u8[16];  /**< 16 unsigned 8 bit lanes. */
  i16 s16[8];      /**< 8 signed 16 bit lanes. */
  uint16_t u16[8]; /**< 8 unsigned 16 bit lanes. */
  i32 s32[4];      /**< 4 signed 32 bit lanes. */
  uint32_t u32[4]; /**< 4 unsigned 32 bit lanes. */
  i64 s64[2];      /**< 2 signed 64 bit lanes. */
  uint64_t u64[2]; /**< 2 unsigned 64 bit lanes. */
  float f32[4];    /**< 4 32 bit float lanes. */
  double f64[2];   /**< 2 64 bit float lanes. */
};

constexpr u32 kRoundNearest = 0x00;    /**< Rounding mode: to nearest even. */
constexpr u32 kRoundTowardZero = 0x01; /**< Rounding mode: toward zero. */
constexpr u32 kRoundUp = 0x02;         /**< Rounding mode: toward positive infinity. */
constexpr u32 kRoundDown = 0x03;       /**< Rounding mode: toward negative infinity. */
constexpr u32 kRoundMask = 0x03;       /**< Mask of the rounding mode bits. */

/** Floating point status and control register (FPSCR). */
struct FPSCRRegister {
  u32 csr; /**< Host floating point control word as last read or written. */

  /** Host specific access to the floating point control word. */
  using Platform = platform::FPSCRPlatform;
  /** Bit position of the host rounding field in csr. */
  static constexpr size_t kRoundShift = Platform::kRoundShift;
  /** Mask of the host rounding field in csr. */
  static constexpr size_t kRoundMaskVal = Platform::kRoundMaskVal;
  /** Mask of the host flush to zero and denormals are zero bits in csr. */
  static constexpr size_t kFlushMask = Platform::kFlushMask;

  static_assert(
      [] {
        for (u32 rn = 0; rn <= kRoundMask; ++rn) {
          if (Platform::kHostToGuest[(Platform::kGuestToHost[rn] & kRoundMaskVal) >> kRoundShift] !=
              rn) {
            return false;
          }
        }
        return true;
      }(),
      "kHostToGuest must invert kGuestToHost");

  /** Bits the guest owns, the rest is host policy set by InitHost. */
  static constexpr u32 kGuestMask = static_cast<u32>(kRoundMaskVal) | static_cast<u32>(kFlushMask);

  /**
   * Returns the host floating point control register.
   *
   * @return The MXCSR on x86 and the FPCR on ARM.
   */
  u32 GetCSR() noexcept { return Platform::GetCSR(); }

  /**
   * Writes the host floating point control register.
   *
   * @param value The MXCSR on x86 and the FPCR on ARM.
   */
  void SetCSR(u32 value) noexcept { Platform::SetCSR(value); }

  /**
   * Restores the guest owned bits from a saved word. Restoring the whole word would unmask every
   * FP exception when csr is 0.
   *
   * @param saved The saved csr word.
   */
  void RestoreGuestBits(u32 saved) noexcept {
    csr = (GetCSR() & ~kGuestMask) | (saved & kGuestMask);
    SetCSR(csr);
  }

  /**
   * Reads the host control word into csr.
   *
   * @return The guest rounding mode matching the host rounding mode.
   */
  u32 LoadFromHost() noexcept {
    csr = GetCSR();
    return Platform::kHostToGuest[(csr & kRoundMaskVal) >> kRoundShift];
  }

  /**
   * Sets the host rounding mode from a guest rounding mode and writes csr to the host.
   *
   * @param value The guest rounding mode, one of the kRound constants.
   */
  void StoreToHost(u32 value) noexcept {
    csr &= ~kRoundMaskVal;
    csr |= Platform::kGuestToHost[value & kRoundMask];
    SetCSR(csr);
  }

  /** Turns on flush to zero mode and writes csr to the host without checking its current state. */
  void EnableFlushModeUnconditional() noexcept {
    csr |= kFlushMask;
    SetCSR(csr);
  }

  /** Turns off flush to zero mode and writes csr to the host without checking its current state. */
  void DisableFlushModeUnconditional() noexcept {
    csr &= ~kFlushMask;
    SetCSR(csr);
  }

  /** Turns on flush to zero mode, writing csr to the host only when the mode was off. */
  void EnableFlushMode() noexcept {
    if ((csr & kFlushMask) != kFlushMask) [[unlikely]] {
      csr |= kFlushMask;
      SetCSR(csr);
    }
  }

  /** Turns off flush to zero mode, writing csr to the host only when the mode was on. */
  void DisableFlushMode() noexcept {
    if ((csr & kFlushMask) != 0) [[unlikely]] {
      csr &= ~kFlushMask;
      SetCSR(csr);
    }
  }

  /**
   * Sets flush to zero mode, writing csr to the host only when the mode changes.
   *
   * @param enable True to turn flush to zero on, false to turn it off.
   */
  void SetFlushMode(bool enable) noexcept {
    if (enable) {
      EnableFlushMode();
    } else {
      DisableFlushMode();
    }
  }

  /** Reads the host control word, applies the host exception policy and writes it back. */
  void InitHost() noexcept {
    csr = GetCSR();
    Platform::InitHostExceptions(csr);
    SetCSR(csr);
  }
};

}  // namespace rex::ppc

/** Global alias of rex::ppc::Register used by recompiled code. */
using PPCRegister = rex::ppc::Register;
/** Global alias of rex::ppc::XERRegister used by recompiled code. */
using PPCXERRegister = rex::ppc::XERRegister;
/** Global alias of rex::ppc::CRRegister used by recompiled code. */
using PPCCRRegister = rex::ppc::CRRegister;
/** Global alias of rex::ppc::VRegister used by recompiled code. */
using PPCVRegister = rex::ppc::VRegister;
/** Global alias of rex::ppc::FPSCRRegister used by recompiled code. */
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

/** PPC execution context of a thread. */
struct alignas(0x40) PPCContext {
  /** General purpose register 3, the first integer argument and the integer return value. */
  PPCRegister r3;
  /**
   * General purpose registers r0 to r31 except r3, 64 bit. r1 is the stack pointer, r4 to r10 are
   * the integer arguments after r3, r13 is the KPCR pointer and r14 to r31 are saved across calls.
   * @{
   */
  PPCRegister r0;
  PPCRegister r1;
  PPCRegister r2;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;
  /** @} */

  u64 lr;             /**< Link register, the return address of the current call. */
  PPCRegister ctr;    /**< Count register, the target of indirect branches. */
  PPCXERRegister xer; /**< Fixed point exception register. */
  /** Value loaded by the last load reserved, which the paired store conditional compares. */
  PPCRegister reserved;
  /** Machine state register, initially 0x200A000, mtmsrd writes only the bits under 0x8020. */
  u32 msr = 0x200A000;
  PPCCRRegister cr0;      /**< Condition register field 0, CR bits 0 to 3. */
  PPCCRRegister cr1;      /**< Condition register field 1, CR bits 4 to 7. */
  PPCCRRegister cr2;      /**< Condition register field 2, CR bits 8 to 11. */
  PPCCRRegister cr3;      /**< Condition register field 3, CR bits 12 to 15. */
  PPCCRRegister cr4;      /**< Condition register field 4, CR bits 16 to 19. */
  PPCCRRegister cr5;      /**< Condition register field 5, CR bits 20 to 23. */
  PPCCRRegister cr6;      /**< Condition register field 6, CR bits 24 to 27. */
  PPCCRRegister cr7;      /**< Condition register field 7, CR bits 28 to 31. */
  PPCFPSCRRegister fpscr; /**< Floating point status and control register. */
  u8 vscr_sat = 0;        /**< VSCR saturation flag for vector ops. */
  u8 vscr_nj = 0;         /**< VSCR non Java mode bit as mtvscr stored it. */

  /** Last indirect call target address. Set by REX_CALL_INDIRECT_FUNC before dispatch. */
  u32 last_indirect_target = 0;

  /** The thread state that owns this register file, null for one a host built. */
  rex::runtime::ThreadState* thread_state = nullptr;

  /** The innermost detour running on this register file. */
  rex::ppc::detail::DetourFrame* detour_frame = nullptr;

  /**
   * Floating point registers f0 to f31, 64 bit. f1 to f13 are the float arguments, f1 is the float
   * return value and f14 to f31 are saved across calls.
   * @{
   */
  PPCRegister f0;
  PPCRegister f1;
  PPCRegister f2;
  PPCRegister f3;
  PPCRegister f4;
  PPCRegister f5;
  PPCRegister f6;
  PPCRegister f7;
  PPCRegister f8;
  PPCRegister f9;
  PPCRegister f10;
  PPCRegister f11;
  PPCRegister f12;
  PPCRegister f13;
  PPCRegister f14;
  PPCRegister f15;
  PPCRegister f16;
  PPCRegister f17;
  PPCRegister f18;
  PPCRegister f19;
  PPCRegister f20;
  PPCRegister f21;
  PPCRegister f22;
  PPCRegister f23;
  PPCRegister f24;
  PPCRegister f25;
  PPCRegister f26;
  PPCRegister f27;
  PPCRegister f28;
  PPCRegister f29;
  PPCRegister f30;
  PPCRegister f31;
  /** @} */

  /** Vector registers v0 to v31, 128 bit, v14 to v31 saved across calls. @{ */
  PPCVRegister v0;
  PPCVRegister v1;
  PPCVRegister v2;
  PPCVRegister v3;
  PPCVRegister v4;
  PPCVRegister v5;
  PPCVRegister v6;
  PPCVRegister v7;
  PPCVRegister v8;
  PPCVRegister v9;
  PPCVRegister v10;
  PPCVRegister v11;
  PPCVRegister v12;
  PPCVRegister v13;
  PPCVRegister v14;
  PPCVRegister v15;
  PPCVRegister v16;
  PPCVRegister v17;
  PPCVRegister v18;
  PPCVRegister v19;
  PPCVRegister v20;
  PPCVRegister v21;
  PPCVRegister v22;
  PPCVRegister v23;
  PPCVRegister v24;
  PPCVRegister v25;
  PPCVRegister v26;
  PPCVRegister v27;
  PPCVRegister v28;
  PPCVRegister v29;
  PPCVRegister v30;
  PPCVRegister v31;
  /** @} */
  /** VMX128 vector registers v32 to v127, 128 bit, present only with the PPC VMX128 extension, v64
   * to v127 saved across calls. @{ */
  PPCVRegister v32;
  PPCVRegister v33;
  PPCVRegister v34;
  PPCVRegister v35;
  PPCVRegister v36;
  PPCVRegister v37;
  PPCVRegister v38;
  PPCVRegister v39;
  PPCVRegister v40;
  PPCVRegister v41;
  PPCVRegister v42;
  PPCVRegister v43;
  PPCVRegister v44;
  PPCVRegister v45;
  PPCVRegister v46;
  PPCVRegister v47;
  PPCVRegister v48;
  PPCVRegister v49;
  PPCVRegister v50;
  PPCVRegister v51;
  PPCVRegister v52;
  PPCVRegister v53;
  PPCVRegister v54;
  PPCVRegister v55;
  PPCVRegister v56;
  PPCVRegister v57;
  PPCVRegister v58;
  PPCVRegister v59;
  PPCVRegister v60;
  PPCVRegister v61;
  PPCVRegister v62;
  PPCVRegister v63;
  PPCVRegister v64;
  PPCVRegister v65;
  PPCVRegister v66;
  PPCVRegister v67;
  PPCVRegister v68;
  PPCVRegister v69;
  PPCVRegister v70;
  PPCVRegister v71;
  PPCVRegister v72;
  PPCVRegister v73;
  PPCVRegister v74;
  PPCVRegister v75;
  PPCVRegister v76;
  PPCVRegister v77;
  PPCVRegister v78;
  PPCVRegister v79;
  PPCVRegister v80;
  PPCVRegister v81;
  PPCVRegister v82;
  PPCVRegister v83;
  PPCVRegister v84;
  PPCVRegister v85;
  PPCVRegister v86;
  PPCVRegister v87;
  PPCVRegister v88;
  PPCVRegister v89;
  PPCVRegister v90;
  PPCVRegister v91;
  PPCVRegister v92;
  PPCVRegister v93;
  PPCVRegister v94;
  PPCVRegister v95;
  PPCVRegister v96;
  PPCVRegister v97;
  PPCVRegister v98;
  PPCVRegister v99;
  PPCVRegister v100;
  PPCVRegister v101;
  PPCVRegister v102;
  PPCVRegister v103;
  PPCVRegister v104;
  PPCVRegister v105;
  PPCVRegister v106;
  PPCVRegister v107;
  PPCVRegister v108;
  PPCVRegister v109;
  PPCVRegister v110;
  PPCVRegister v111;
  PPCVRegister v112;
  PPCVRegister v113;
  PPCVRegister v114;
  PPCVRegister v115;
  PPCVRegister v116;
  PPCVRegister v117;
  PPCVRegister v118;
  PPCVRegister v119;
  PPCVRegister v120;
  PPCVRegister v121;
  PPCVRegister v122;
  PPCVRegister v123;
  PPCVRegister v124;
  PPCVRegister v125;
  PPCVRegister v126;
  PPCVRegister v127;
  /** @} */

  /**
   * Packs cr0 to cr7 into the 32 bit CR layout.
   *
   * @return The CR word, cr0 in the top nibble.
   */
  rex::u32 GetCR() const noexcept {
    return (cr0.GetRaw() << 28) | (cr1.GetRaw() << 24) | (cr2.GetRaw() << 20) |
           (cr3.GetRaw() << 16) | (cr4.GetRaw() << 12) | (cr5.GetRaw() << 8) | (cr6.GetRaw() << 4) |
           cr7.GetRaw();
  }

  /**
   * Unpacks a 32 bit CR word into cr0 to cr7.
   *
   * @param value The CR word, cr0 in the top nibble.
   */
  void SetCR(rex::u32 value) noexcept {
    cr0.SetRaw(value >> 28);
    cr1.SetRaw(value >> 24);
    cr2.SetRaw(value >> 20);
    cr3.SetRaw(value >> 16);
    cr4.SetRaw(value >> 12);
    cr5.SetRaw(value >> 8);
    cr6.SetRaw(value >> 4);
    cr7.SetRaw(value);
  }

  /**
   * Number of condition register fields that survive a call, cr2 to cr5.
   */
  static constexpr size_t kNonVolatileCRCount = 4;

  /**
   * Size in bytes of the non volatile save area, 1620 bytes in total. The layout holds these
   * parts in order.
   * - r14-r31 (144)
   * - f14-f31 (144)
   * - v14-v31 (288)
   * - v64-v127 (1024)
   * - cr2-cr5 (16)
   * - fpscr (4)
   */
  static constexpr size_t kNonVolatileSaveSize =
      18 * sizeof(PPCRegister) + 18 * sizeof(PPCRegister) + 18 * sizeof(PPCVRegister) +
      64 * sizeof(PPCVRegister) + kNonVolatileCRCount * sizeof(PPCCRRegister) +
      sizeof(PPCFPSCRRegister);

  /**
   * Copies the non volatile registers into a buffer of kNonVolatileSaveSize bytes.
   *
   * @param dst The destination buffer.
   */
  void SaveNonVolatiles(u8* dst) const {
    std::memcpy(dst, &r14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &f14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &v14, 18 * sizeof(PPCVRegister));
    dst += 18 * sizeof(PPCVRegister);
    std::memcpy(dst, &v64, 64 * sizeof(PPCVRegister));
    dst += 64 * sizeof(PPCVRegister);
    std::memcpy(dst, &cr2, kNonVolatileCRCount * sizeof(PPCCRRegister));
    dst += kNonVolatileCRCount * sizeof(PPCCRRegister);
    std::memcpy(dst, &fpscr, sizeof(PPCFPSCRRegister));
  }

  /**
   * Loads the non volatile registers from a buffer written by SaveNonVolatiles. Only the title
   * owned bits of fpscr are restored.
   *
   * @param src The source buffer of kNonVolatileSaveSize bytes.
   */
  void RestoreNonVolatiles(const u8* src) {
    std::memcpy(&r14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&f14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&v14, src, 18 * sizeof(PPCVRegister));
    src += 18 * sizeof(PPCVRegister);
    std::memcpy(&v64, src, 64 * sizeof(PPCVRegister));
    src += 64 * sizeof(PPCVRegister);
    std::memcpy(&cr2, src, kNonVolatileCRCount * sizeof(PPCCRRegister));
    src += kNonVolatileCRCount * sizeof(PPCCRRegister);
    PPCFPSCRRegister saved_fpscr;
    std::memcpy(&saved_fpscr, src, sizeof(PPCFPSCRRegister));
    fpscr.RestoreGuestBits(saved_fpscr.csr);
  }
};
static_assert(offsetof(PPCContext, cr5) - offsetof(PPCContext, cr2) ==
                  (PPCContext::kNonVolatileCRCount - 1) * sizeof(PPCCRRegister),
              "cr2 to cr5 must be contiguous for the non volatile save area");
