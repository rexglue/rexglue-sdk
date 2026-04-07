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

#include <atomic>
#include <bit>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <unordered_map>

#include <rex/chrono/clock.h>  // For mftb timebase access
#include <rex/logging.h>
#include <rex/platform/fpscr.h>
#include <rex/ppc/memory.h>
#include <rex/types.h>
#include <rex/thread/mutex.h>

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

//=============================================================================
// Compiler-Specific Intrinsics
//=============================================================================

#if defined(__clang__)
// Clang has builtin rotate functions and debugtrap
#elif defined(__GNUC__)
// GCC doesn't have __builtin_rotateleft*, use C++20 std::rotl
#ifndef __builtin_rotateleft32
#define __builtin_rotateleft32(x, n) std::rotl(static_cast<uint32_t>(x), static_cast<int>(n))
#endif
#ifndef __builtin_rotateleft64
#define __builtin_rotateleft64(x, n) std::rotl(static_cast<uint64_t>(x), static_cast<int>(n))
#endif
// GCC doesn't have __builtin_debugtrap, use __builtin_trap or int3 on x86
#ifndef __builtin_debugtrap
#if defined(__x86_64__) || defined(__i386__)
#define __builtin_debugtrap() __asm__ __volatile__("int3")
#else
#define __builtin_debugtrap() __builtin_trap()
#endif
#endif
#endif

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)
#define REX_WEAK_FUNC(x) __attribute__((weak, noinline)) REX_FUNC(x)

// Compiler-specific assume hint for alignment, with optional Tracy zone
#if defined(REXGLUE_PROFILE_GUEST_FUNCTIONS) && defined(REXGLUE_ENABLE_PROFILING)
#include <tracy/Tracy.hpp>
#if defined(__clang__)
#define REX_FUNC_PROLOGUE()                     \
  __builtin_assume(((size_t)base & 0x1F) == 0); \
  ZoneNamedN(___tracy_guest_zone, __func__, true)
#elif defined(__GNUC__)
#define REX_FUNC_PROLOGUE()         \
  do {                              \
    if (((size_t)base & 0x1F) != 0) \
      __builtin_unreachable();      \
  } while (0);                      \
  ZoneNamedN(___tracy_guest_zone, __func__, true)
#else
#define REX_FUNC_PROLOGUE() ZoneNamedN(___tracy_guest_zone, __func__, true)
#endif
#else
// Original alignment-hint-only expansion
#if defined(__clang__)
#define REX_FUNC_PROLOGUE() __builtin_assume(((size_t)base & 0x1F) == 0)
#elif defined(__GNUC__)
#define REX_FUNC_PROLOGUE()         \
  do {                              \
    if (((size_t)base & 0x1F) != 0) \
      __builtin_unreachable();      \
  } while (0)
#else
#define REX_FUNC_PROLOGUE() ((void)0)
#endif
#endif

#ifndef REX_CALL_FUNC
#define REX_CALL_FUNC(x) x(ctx, base)
#endif

//=============================================================================
// Library Mode Stubs
//=============================================================================
// Safe fallbacks when ppc_config.h is not included.

#if !defined(PPC_CONFIG_H_INCLUDED) && !defined(REX_CONFIG_H_INCLUDED)

#define REX_LOOKUP_FUNC(x, y) ((PPCFunc*)nullptr)

#define REX_CALL_INDIRECT_FUNC(x) __builtin_debugtrap()

#endif  // !PPC_CONFIG_H_INCLUDED && !REX_CONFIG_H_INCLUDED

//=============================================================================
// Recompiled Code Mode
//=============================================================================
// Requires ppc_config.h to be included first.

#if defined(PPC_CONFIG_H_INCLUDED) || defined(REX_CONFIG_H_INCLUDED)

// Bridge old config names to new (for pre-regen generated configs)
#if defined(PPC_IMAGE_BASE) && !defined(REX_IMAGE_BASE)
#define REX_IMAGE_BASE PPC_IMAGE_BASE
#define REX_IMAGE_SIZE PPC_IMAGE_SIZE
#define REX_CODE_BASE PPC_CODE_BASE
#define REX_CODE_SIZE PPC_CODE_SIZE
#endif

// Function table lookup: indexed by (addr - CODE_BASE)
#undef REX_LOOKUP_FUNC
#define REX_LOOKUP_FUNC(x, y) \
  (*(PPCFunc**)(x + REX_IMAGE_BASE + REX_IMAGE_SIZE + (uint64_t(uint32_t(y) - REX_CODE_BASE) * 2)))

#undef REX_CALL_INDIRECT_FUNC
#include <rex/perf/counter.h>
#define REX_CALL_INDIRECT_FUNC(x) \
  PROFILE_FUNCTION_DISPATCHED();  \
  REX_LOOKUP_FUNC(base, x)(ctx, base);

#endif  // PPC_CONFIG_H_INCLUDED || REX_CONFIG_H_INCLUDED

//=============================================================================
// Unimplemented Instruction Exception
//=============================================================================

#ifndef REX_UNIMPLEMENTED
#define REX_UNIMPLEMENTED(addr, opcode) \
  throw std::runtime_error("Unimplemented PPC instruction: " opcode)
#endif

//=============================================================================
// Timebase Access
//=============================================================================

#define REX_QUERY_TIMEBASE() rex::chrono::Clock::QueryGuestTickCount()

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

namespace rex {

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

}  // namespace rex

using PPCRegister = rex::Register;
using PPCXERRegister = rex::XERRegister;
using PPCCRRegister = rex::CRRegister;
using PPCVRegister = rex::VRegister;
using PPCFPSCRRegister = rex::FPSCRRegister;

#define PPC_ROUND_NEAREST rex::kRoundNearest
#define PPC_ROUND_TOWARD_ZERO rex::kRoundTowardZero
#define PPC_ROUND_UP rex::kRoundUp
#define PPC_ROUND_DOWN rex::kRoundDown
#define PPC_ROUND_MASK rex::kRoundMask

//=============================================================================
// PPCContext Structure
//=============================================================================

struct alignas(0x40) PPCContext {
  PPCRegister r3;
#if !defined(PPC_CONFIG_NON_ARGUMENT_AS_LOCAL) && !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
  PPCRegister r0;
#endif
  PPCRegister r1;
#if !defined(PPC_CONFIG_NON_ARGUMENT_AS_LOCAL) && !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
  PPCRegister r2;
#endif
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
#if !defined(PPC_CONFIG_NON_ARGUMENT_AS_LOCAL) && !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
  PPCRegister r11;
  PPCRegister r12;
#endif
  PPCRegister r13;
#if !defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL) && !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
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
#endif

#if !defined(PPC_CONFIG_SKIP_LR) && !defined(REX_CONFIG_SKIP_LR)
  uint64_t lr;
#endif
#if !defined(PPC_CONFIG_CTR_AS_LOCAL) && !defined(REX_CONFIG_CTR_AS_LOCAL)
  PPCRegister ctr;
#endif
#if !defined(PPC_CONFIG_XER_AS_LOCAL) && !defined(REX_CONFIG_XER_AS_LOCAL)
  PPCXERRegister xer;
#endif
#if !defined(PPC_CONFIG_RESERVED_AS_LOCAL) && !defined(REX_CONFIG_RESERVED_AS_LOCAL)
  PPCRegister reserved;
#endif
#if !defined(PPC_CONFIG_SKIP_MSR) && !defined(REX_CONFIG_SKIP_MSR)
  uint32_t msr = 0x200A000;
#endif
#if !defined(PPC_CONFIG_CR_AS_LOCAL) && !defined(REX_CONFIG_CR_AS_LOCAL)
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
#endif
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;  // VSCR saturation flag (for vector ops)

#if !defined(PPC_CONFIG_NON_ARGUMENT_AS_LOCAL) && !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
  PPCRegister f0;
#endif
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
#if !defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL) && !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
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
#endif

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
#if !defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL) && !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
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
#endif
#if !defined(PPC_CONFIG_NON_ARGUMENT_AS_LOCAL) && !defined(REX_CONFIG_NON_ARGUMENT_AS_LOCAL)
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
#endif
#if !defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL) && !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
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
#endif

#if !defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL) && !defined(REX_CONFIG_NON_VOLATILE_AS_LOCAL)
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
#else
  static constexpr size_t kNonVolatileSaveSize = 0;
  inline void SaveNonVolatiles(uint8_t*) const {}
  inline void RestoreNonVolatiles(const uint8_t*) {}
#endif
};

//=============================================================================
// PPC setjmp/longjmp Support
//=============================================================================
// Native setjmp/longjmp with host-side jmp_buf storage.
// Guest jmp_buf address is used as a key, not for actual storage.
//
// Problem: Xbox 360's jmp_buf format stores PPC registers (GPRs, LR, CR, etc.)
// but native x86-64 jmp_buf expects x86-64 registers (RBP, RSP, RBX, etc.).
// Using guest memory as jmp_buf storage causes crashes.
//
// Solution: Use the guest jmp_buf address as a key into a host-side map that
// stores the actual x86-64 jmp_buf. The guest memory is ignored.

namespace rex {

// Thread-local storage for jmp_buf mapping
// Maps guest jmp_buf address -> host jmp_buf
inline std::unordered_map<uint32_t, jmp_buf>& get_jmp_buf_map() {
  static thread_local std::unordered_map<uint32_t, jmp_buf> map;
  return map;
}

// Custom setjmp - uses guest address as key, stores in host map
// Returns 0 on initial call, non-zero value from longjmp on return
// NOTE: Must be a macro so setjmp captures the caller's stack frame.
// An inline function wrapper causes MSVC debug builds to crash because
// setjmp saves the wrapper's frame, which is gone by the time longjmp fires.
#define ppc_setjmp(guest_buf_addr) (setjmp(::rex::get_jmp_buf_map()[(guest_buf_addr)]))

// Custom longjmp - looks up host jmp_buf by guest address
// Never returns - jumps back to the corresponding setjmp site
[[noreturn]] inline void ppc_longjmp(uint32_t guest_buf_addr, int val) {
  auto& map = get_jmp_buf_map();
  auto it = map.find(guest_buf_addr);
  if (it != map.end()) {
    longjmp(it->second, val);
  }
  // setjmp was never called for this address - abort
  std::abort();
}

}  // namespace rex

//=============================================================================
// PPC Interrupt and Exception Handling
//=============================================================================

// Global lock count storage - tracks nesting depth
inline std::atomic<int32_t>& ppc_global_lock_count_() {
  static std::atomic<int32_t> count{0};
  return count;
}

// Check global lock state (for mfmsr)
// Returns 0x8000 if unlocked (interrupts enabled), 0 if locked
#define REX_CHECK_GLOBAL_LOCK()                                        \
  ([&]() -> uint64_t {                                                 \
    auto lock_ = rex::thread::global_critical_region::AcquireDirect(); \
    return ppc_global_lock_count_().load() ? 0 : 0x8000;               \
  }())

// Enter global lock (for mtmsrd from r13)
#define REX_ENTER_GLOBAL_LOCK()                          \
  do {                                                   \
    rex::thread::global_critical_region::mutex().lock(); \
    ppc_global_lock_count_().fetch_add(1);               \
  } while (0)

// Leave global lock (for mtmsrd from non-r13)
#define REX_LEAVE_GLOBAL_LOCK()                                                           \
  do {                                                                                    \
    auto old_count_ = ppc_global_lock_count_().fetch_sub(1);                              \
    assert(old_count_ >= 1 && "LeaveGlobalLock called without matching EnterGlobalLock"); \
    rex::thread::global_critical_region::mutex().unlock();                                \
  } while (0)

//=============================================================================
// PPC Trap Handling
//=============================================================================
// Trap instructions (tw/twi/td/tdi) generate a Program Exception on PPC.
// The kernel inspects the trap type and dispatches to the appropriate handler.
// Unconditional traps (twi 31, r0, <imm>) encode a service code in the immediate:
//   20, 26 = Debug print (r3 = string ptr, r4 = length)
//   0, 22  = Debug break
//   25     = No-op
// Conditional traps are inline assertions that continue on the exception return path.
inline void ppc_trap(PPCContext& ctx, uint8_t* base, uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      auto str = PPC_LOAD_STRING(ctx.r3.u32, ctx.r4.u16);
      REXCPU_DEBUG("(service trap) {}", str);
      break;
    }
    case 0:
    case 22:
      REXCPU_WARN("tw/td trap hit (type {})", trap_type);
      break;
    case 25:
      break;
    default:
      REXCPU_WARN("Unknown trap type {}", trap_type);
      break;
  }
}

//=============================================================================
// Legacy Compat Aliases
//=============================================================================
// Old PPC_ names map to new REX_ names. Consumed by generated code until
// the next codegen run. Remove these once all downstream projects are
// regenerated with updated templates.

#define PPC_FUNC(x) REX_FUNC(x)
#define PPC_FUNC_IMPL(x) REX_EXTERN(x)
#define PPC_EXTERN_FUNC(x) REX_EXTERN(x)
#define PPC_EXTERN_IMPORT(x) REX_EXTERN(x)
#define PPC_WEAK_FUNC(x) REX_WEAK_FUNC(x)
#define PPC_JOIN(x, y) REX_JOIN(x, y)
#define PPC_XSTRINGIFY(x) REX_XSTRINGIFY(x)
#define PPC_STRINGIFY(x) REX_STRINGIFY(x)
#define PPC_FUNC_PROLOGUE REX_FUNC_PROLOGUE
#define PPC_CALL_FUNC(x) REX_CALL_FUNC(x)
#define PPC_LOOKUP_FUNC(x, y) REX_LOOKUP_FUNC(x, y)
#define PPC_CALL_INDIRECT_FUNC(x) REX_CALL_INDIRECT_FUNC(x)
#define PPC_UNIMPLEMENTED(addr, opcode) REX_UNIMPLEMENTED(addr, opcode)
#define PPC_QUERY_TIMEBASE() REX_QUERY_TIMEBASE()
#define PPC_CHECK_GLOBAL_LOCK() REX_CHECK_GLOBAL_LOCK()
#define PPC_ENTER_GLOBAL_LOCK() REX_ENTER_GLOBAL_LOCK()
#define PPC_LEAVE_GLOBAL_LOCK() REX_LEAVE_GLOBAL_LOCK()
