/**
 * @file        ppc/context.h
 * @brief       PPC thread context, guest function macros, and interrupt handling
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp PPCContext infrastructure
 */

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/types.h>

#include <simde/x86/avx.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

//=============================================================================
// PPCFunc Type Definition
//=============================================================================
// Function signature for recompiled PPC functions.
// All recompiled functions take a context reference and memory base pointer.

// Forward declaration of the PPC execution context
struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);
}  // namespace rex::runtime

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)
#define REX_WEAK_FUNC(x) __attribute__((weak, noinline)) REX_FUNC(x)

//=============================================================================
// Function Mapping
//=============================================================================

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];

//=============================================================================
// Pack/Unpack Constants (NORMPACKED32 - 2:10:10:10 format)
//=============================================================================

constexpr float kPack2101010_Min10 = std::bit_cast<float>(0x403FFE01u);
constexpr float kPack2101010_Max10 = std::bit_cast<float>(0x404001FFu);
constexpr float kPack2101010_Min2 = std::bit_cast<float>(0x40400000u);
constexpr float kPack2101010_Max2 = std::bit_cast<float>(0x40400003u);

namespace rex::ppc {

//=============================================================================
// General Purpose Register
//=============================================================================

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

//=============================================================================
// Fixed-Point Exception Register (XER)
//=============================================================================

struct XERRegister {
  uint8_t so;
  uint8_t ov;
  uint8_t ca;
};

//=============================================================================
// Condition Register (CR) Field
//=============================================================================

struct CRRegister {
  uint8_t lt;
  uint8_t gt;
  uint8_t eq;
  union {
    uint8_t so;
    uint8_t un;
  };

  inline uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  inline void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  inline void compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  inline void compare(double left, double right) noexcept {
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

//=============================================================================
// Vector Register (128-bit)
//=============================================================================

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

//=============================================================================
// Floating-Point Status and Control Register (FPSCR)
//=============================================================================

constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

struct FPSCRRegister {
  uint32_t csr;

  static constexpr size_t HostToGuest[] = {kRoundNearest, kRoundDown, kRoundUp, kRoundTowardZero};

  using Platform = platform::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }
  inline void setcsr(uint32_t csr) noexcept { Platform::setcsr(csr); }

  inline uint32_t loadFromHost() noexcept {
    csr = getcsr();
    return HostToGuest[(csr & RoundMaskVal) >> RoundShift];
  }

  inline void storeFromGuest(uint32_t value) noexcept {
    csr &= ~RoundMaskVal;
    csr |= Platform::GuestToHost[value & kRoundMask];
    setcsr(csr);
  }

  inline void enableFlushModeUnconditional() noexcept {
    csr |= FlushMask;
    setcsr(csr);
  }

  inline void disableFlushModeUnconditional() noexcept {
    csr &= ~FlushMask;
    setcsr(csr);
  }

  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if ((csr & FlushMask) != 0) [[unlikely]] {
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }

  inline void InitHost() noexcept {
    csr = getcsr();
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
};

// Xenon vmsum3fp128/vmsum4fp128 do NOT return +/-inf when the single-precision
// dot product overflows — they return QNaN. Xenia emulates this deliberately
// (double-precision accumulation plus an MXCSR overflow check that substitutes
// QNaN) with the note "this implementation is accurate, it matches the results
// of xb360 vmsum3", and it is on by default there.
//
// A plain simde_mm_dp_ps gets this wrong: it computes in single precision and
// saturates to +inf. FH1's GJK depends on the QNaN. CSimplex seeds its search
// direction to -FLT_MAX precisely so the first dot3 overflows; the resulting
// QNaN makes the convergence compare unordered (all lanes false), which lets
// the seeding iteration fall through and the solver run. With +inf the compare
// is an ordinary finite >= -inf, which is true, so CGJK::TerminateCondition
// returns 1 on iteration 1 of every call and convex-vs-convex collision dies
// (cars and props pass straight through each other).
//
// Accumulating in double cannot overflow for finite float inputs (FLT_MAX^2 is
// ~1.16e77, well inside double range), so the single-precision overflow test is
// simply a range check on the double result. NaN inputs propagate to QNaN via
// the same negated comparison.
inline float dotProductToGuestFloat(double acc) noexcept {
  constexpr double kFltMax = 3.4028234663852886e+38;
  // Negated so that a NaN acc (from a NaN input) also takes the QNaN path.
  if (!(acc >= -kFltMax && acc <= kFltMax)) {
    return std::bit_cast<float>(uint32_t(0x7FC00000));
  }
  const float result = static_cast<float>(acc);
  // Xenia unconditionally flushes denormal dot-product outputs to zero; the
  // scalar math here is not subject to the host FTZ mode the old dp_ps path
  // relied on, so do it explicitly.
  if (result != 0.0f && (std::bit_cast<uint32_t>(result) & 0x7F800000u) == 0u) {
    return std::bit_cast<float>(std::bit_cast<uint32_t>(result) & 0x80000000u);
  }
  return result;
}

// Guest vector elements are stored reversed relative to host lanes, so guest
// x,y,z are host lanes 1,2,3 — this is why the old dp_ps mask was 0xEF and not
// 0x7F. Element w (host lane 0) is excluded.
inline simde__m128 vmsum3fp(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float av[4], bv[4];
  simde_mm_store_ps(av, a);
  simde_mm_store_ps(bv, b);
  const double acc = static_cast<double>(av[1]) * static_cast<double>(bv[1]) +
                     static_cast<double>(av[2]) * static_cast<double>(bv[2]) +
                     static_cast<double>(av[3]) * static_cast<double>(bv[3]);
  return simde_mm_set1_ps(dotProductToGuestFloat(acc));
}

inline simde__m128 vmsum4fp(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float av[4], bv[4];
  simde_mm_store_ps(av, a);
  simde_mm_store_ps(bv, b);
  const double acc = static_cast<double>(av[0]) * static_cast<double>(bv[0]) +
                     static_cast<double>(av[1]) * static_cast<double>(bv[1]) +
                     static_cast<double>(av[2]) * static_cast<double>(bv[2]) +
                     static_cast<double>(av[3]) * static_cast<double>(bv[3]);
  return simde_mm_set1_ps(dotProductToGuestFloat(acc));
}

}  // namespace rex::ppc

using PPCRegister = rex::ppc::Register;
using PPCXERRegister = rex::ppc::XERRegister;
using PPCCRRegister = rex::ppc::CRRegister;
using PPCVRegister = rex::ppc::VRegister;
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

//=============================================================================
// PPCContext Structure
//=============================================================================

struct alignas(0x40) PPCContext {
  PPCRegister r3;
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

  uint64_t lr;
  PPCRegister ctr;
  PPCXERRegister xer;
  PPCRegister reserved;
  uint32_t msr = 0x200A000;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;  // VSCR saturation flag (for vector ops)

  /**
   * Last indirect call target address. Set by REX_CALL_INDIRECT_FUNC before
   * dispatch. Used by the invalid-function trap to report the faulting address.
   * Unconditional (not guarded by config flags) because ctr may be optimized
   * to a local variable via REX_CONFIG_CTR_AS_LOCAL.
   */
  uint32_t last_indirect_target = 0;

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

  //--- Non-volatile register save/restore --------
  // Layout: r14-r31 (144) | f14-f31 (144) | v14-v31 (288) | v64-v127 (1024)
  // Total: 1600 bytes.  Buffer must be at least this large.
  static constexpr size_t kNonVolatileSaveSize =
      18 * sizeof(PPCRegister) + 18 * sizeof(PPCRegister) + 18 * sizeof(PPCVRegister) +
      64 * sizeof(PPCVRegister);

  inline void SaveNonVolatiles(uint8_t* dst) const {
    std::memcpy(dst, &r14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &f14, 18 * sizeof(PPCRegister));
    dst += 18 * sizeof(PPCRegister);
    std::memcpy(dst, &v14, 18 * sizeof(PPCVRegister));
    dst += 18 * sizeof(PPCVRegister);
    std::memcpy(dst, &v64, 64 * sizeof(PPCVRegister));
  }

  inline void RestoreNonVolatiles(const uint8_t* src) {
    std::memcpy(&r14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&f14, src, 18 * sizeof(PPCRegister));
    src += 18 * sizeof(PPCRegister);
    std::memcpy(&v14, src, 18 * sizeof(PPCVRegister));
    src += 18 * sizeof(PPCVRegister);
    std::memcpy(&v64, src, 64 * sizeof(PPCVRegister));
  }
};
