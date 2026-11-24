/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_METAL_METAL_ZERO_COPY_CONTRACT_H_
#define XENIA_GPU_METAL_METAL_ZERO_COPY_CONTRACT_H_

#include <cstdint>

namespace rex {
namespace graphics {
namespace metal {

// Device-free vocabulary for the Metal zero-copy shared-memory pre-gate
// (pre-gate-metal-zero-copy-shared-memory tasks 2.1-2.3).
//
// The backend may back its 512 MiB shared-memory buffer either by the
// established dedicated mirror allocation or by wrapping the guest physical
// view itself with newBufferWithBytesNoCopy. Apple's contract for that call is
// specific -- page-aligned pointer, page-aligned length, one contiguous VM
// region -- and each way it can fail has different consequences, so "it did not
// alias" is not a usable answer. Every rejection below is a distinct named
// value that conformance can enumerate exhaustively and that the one startup
// report can print.
//
// Everything here is a pure value transform: no device, no Objective-C, no
// environment access beyond a caller-supplied string. That is deliberate. The
// dangerous inputs (an overflowing address, a length past the device limit, a
// buffer whose contents pointer is not the guest base) are awkward to reach on
// a live device and trivial to enumerate as values. Production builds these
// inputs and calls these functions, so a rule deleted in production fails
// conformance.

// The exact-off diagnostic gate. Only the exact value "1" requests the probe.
inline constexpr const char* kZeroCopyGateEnvVar =
    "XE_METAL_ZERO_COPY_SHARED_MEMORY";
// Control arm for the pre-gate's forced-fallback evidence (tasks 3.3/6.3): when
// the probe is requested AND this is exactly "1", the contract rejects before
// any device call. Without it, the only fallback arm reachable on a machine
// that satisfies the contract is "not requested", which cannot show that the
// rejection path itself lands on the mirror.
inline constexpr const char* kZeroCopyForceRejectEnvVar =
    "XE_METAL_ZERO_COPY_FORCE_REJECT";
// Control arm for the OBSERVER-rejection path (task 5.1). Accepting the alias
// requires arming the in-flight overlap observer, and a host on which that
// allocation fails is not reachable on demand -- so without this the fallback
// rule "an unobservable alias is not accepted" could only be reviewed, never
// run. When the probe is requested AND this is exactly "1", the observer
// refuses to arm, exactly as an allocation failure would.
inline constexpr const char* kZeroCopyForceObserverRejectEnvVar =
    "XE_METAL_ZERO_COPY_FORCE_OBSERVER_REJECT";

// Exact opt-in. A gate that accepted "true", "01", " 1" or any nonempty value
// would make "the mirror is still in use" unfalsifiable from the environment
// alone, which is the one fact every control arm of this pre-gate rests on.
// nullptr (unset) is false.
constexpr bool IsZeroCopyGateEnabled(const char* value) {
  if (!value) {
    return false;
  }
  return value[0] == '1' && value[1] == '\0';
}

// Device-free half of the alias contract. kSatisfied means only that nothing
// observable without a device disqualifies the request; the VM-region, Metal
// allocation, contents-identity and observer-arming steps are separate
// (ZeroCopyDeviceStep).
enum class ZeroCopyPrecondition : uint32_t {
  // No evaluation has happened. Distinct from kSatisfied so a report cannot
  // read "the contract held" from a run that never checked it.
  kNotEvaluated,
  kSatisfied,
  // The forced-fallback control arm was engaged. Not a defect of the request.
  kForcedRejection,
  // memory().TranslatePhysical(0) returned null.
  kNullPointer,
  // A zero-length wrap has no meaning and Metal rejects it.
  kEmptyLength,
  // The host page size is unusable (zero or not a power of two), so neither
  // alignment test below would mean anything.
  kInvalidPageSize,
  kPointerAlignment,
  kLengthAlignment,
  // base + length does not fit in the host address space.
  kAddressOverflow,
  // length exceeds MTLDevice.maxBufferLength.
  kDeviceLimit,
  // Not a value: one past the last real enumerator. Every enumerator must be
  // declared ABOVE this line so the count below, and the conformance arrays
  // pinned to it, follow the enum automatically.
  kCount,
};
inline constexpr uint32_t kZeroCopyPreconditionCount =
    static_cast<uint32_t>(ZeroCopyPrecondition::kCount);

// kCount is a bound, never an observation. Anything at or past it is a value
// the vocabulary does not define, and no derivation may treat it as one.
constexpr bool IsZeroCopyPreconditionValue(ZeroCopyPrecondition value) {
  return static_cast<uint32_t>(value) < kZeroCopyPreconditionCount;
}

struct ZeroCopyContractInputs {
  // Host pointer value of the guest physical base; 0 means null.
  uint64_t base_address = 0;
  uint64_t length = 0;
  uint64_t host_page_size = 0;
  uint64_t device_max_buffer_length = 0;
  // Forced-fallback control arm (kZeroCopyForceRejectEnvVar).
  bool force_rejection = false;
};

// Ordered so that each check only runs once its own inputs are known good: the
// alignment tests are meaningless without a valid page size, and the overflow
// test must precede any use of base + length.
constexpr ZeroCopyPrecondition EvaluateZeroCopyPreconditions(
    const ZeroCopyContractInputs& in) {
  if (in.force_rejection) {
    return ZeroCopyPrecondition::kForcedRejection;
  }
  if (!in.base_address) {
    return ZeroCopyPrecondition::kNullPointer;
  }
  if (!in.length) {
    return ZeroCopyPrecondition::kEmptyLength;
  }
  if (!in.host_page_size ||
      (in.host_page_size & (in.host_page_size - 1)) != 0) {
    return ZeroCopyPrecondition::kInvalidPageSize;
  }
  if (in.base_address % in.host_page_size) {
    return ZeroCopyPrecondition::kPointerAlignment;
  }
  if (in.length % in.host_page_size) {
    return ZeroCopyPrecondition::kLengthAlignment;
  }
  // Overflow-safe: never form base + length before proving it fits. Strict, so
  // an interval ending exactly at 2^64 is rejected too -- its one-past-the-end
  // address is not representable, and both the VM-region check and Metal work
  // in half-open ranges.
  if (in.length > UINT64_MAX - in.base_address) {
    return ZeroCopyPrecondition::kAddressOverflow;
  }
  if (in.length > in.device_max_buffer_length) {
    return ZeroCopyPrecondition::kDeviceLimit;
  }
  return ZeroCopyPrecondition::kSatisfied;
}

// Postcondition on whatever newBufferWithBytesNoCopy returns: the wrap is an
// alias only if the buffer's contents pointer IS the requested base. A buffer
// whose bytes live somewhere else is a copy wearing the no-copy call's name,
// and every later "source and destination are the same memory" decision would
// be wrong about it.
//
// Device-free on purpose. On a conforming host Metal always satisfies this, so
// the check is unreachable there and a device test cannot prove it is
// load-bearing; as a value rule it is enumerable and mutation-killable.
constexpr bool ZeroCopyContentsAreAlias(const void* contents,
                                        const void* requested_base) {
  return contents != nullptr && requested_base != nullptr &&
         contents == requested_base;
}

// Convergence/upload-time identity proof (task 4.2). Suppressing a
// shared-buffer -> guest-RAM copy is sound only when the destination IS the
// source. The installed alias state alone is NOT that proof: it is a startup
// verdict about the base pointer, and a convergence that inferred "no copy
// needed" from the mode would silently publish nothing if the two ever
// disagreed at the range actually being converged. Both conditions are required
// here, so no caller can derive the skip from the mode.
//
// Device-free for the same reason as ZeroCopyContentsAreAlias: on a host that
// accepted the alias the mismatch is unreachable (the buffer's contents ARE the
// guest base, so contents + base == TranslatePhysical(base) for every in-domain
// base), so a device case cannot show the conjunction is load-bearing, while as
// a value rule it is enumerable and mutation-killable.
constexpr bool ZeroCopySelfCopyIsProven(bool alias_installed,
                                        const void* destination,
                                        const void* source) {
  return alias_installed && ZeroCopyContentsAreAlias(destination, source);
}

// The half of the contract that cannot be decided from values alone: the host
// VM map, the Metal device, and -- last -- the observability postcondition.
// Ordered as the initializer performs them, so the enumerator names the FIRST
// step that refused.
enum class ZeroCopyDeviceStep : uint32_t {
  // No device call was made (the gate was off, or a precondition rejected).
  kNotAttempted,
  // The half-open guest interval is not contained in one VM region.
  kVmRegionRejected,
  // newBufferWithBytesNoCopy returned nil.
  kAllocationRejected,
  // A buffer came back, but its contents pointer is not the guest physical
  // base -- i.e. Metal copied or relocated, so it is not an alias.
  kContentsRejected,
  // Metal handed back a genuine alias, but the in-flight CPU rewrite observer
  // could not be armed over it. Aliasing removes the mirror's snapshot, so an
  // alias nobody is watching cannot be distinguished from an alias nobody
  // raced -- absence of evidence would read as permission. The candidate is
  // released and the ordinary mirror is installed instead, which is why this is
  // a REJECTION and not a footnote on kAccepted.
  kObserverRejected,
  // Every step passed, INCLUDING the observer. kAccepted therefore means
  // "aliased and observed"; there is no accepted-but-unwatched state.
  kAccepted,
  // Not a value; see ZeroCopyPrecondition::kCount.
  kCount,
};
inline constexpr uint32_t kZeroCopyDeviceStepCount =
    static_cast<uint32_t>(ZeroCopyDeviceStep::kCount);

constexpr bool IsZeroCopyDeviceStepValue(ZeroCopyDeviceStep value) {
  return static_cast<uint32_t>(value) < kZeroCopyDeviceStepCount;
}

// Facts recorded at three independent points of initialization:
//   * `requested`               -- the environment gate parse
//   * `precondition`/`device_step` -- the attempt itself
//   * `alias_buffer_installed`  -- re-derived from the buffer the shared memory
//                                  actually installed, by comparing its
//                                  contents pointer with the guest base
//
// The last one exists so that the attempt cannot be its own verdict. If the
// report were derived from the attempt alone, a run would say "aliasing" simply
// because it asked to alias and nothing threw. Any disagreement between the
// recorded attempt and the installed backing is reported as
// kInconsistentObservation rather than resolved in favour of either.
struct ZeroCopyObservation {
  bool requested = false;
  ZeroCopyPrecondition precondition = ZeroCopyPrecondition::kNotEvaluated;
  ZeroCopyDeviceStep device_step = ZeroCopyDeviceStep::kNotAttempted;
  bool alias_buffer_installed = false;
};

enum class ZeroCopyStartupReport : uint32_t {
  kNotRequested,
  kPreconditionRejected,
  kVmRegionRejected,
  kAllocationRejected,
  kContentsIdentityRejected,
  // The alias was real but unobservable, so it was released and the mirror
  // installed. Named separately from every other rejection because it is the
  // only one where the DEVICE said yes -- reporting it as kContentsIdentity-
  // Rejected would blame Metal for a decision this backend made.
  kObserverRejected,
  kAliasActive,
  // The recorded facts do not describe one run. Never silently downgraded to a
  // rejection: a disagreement between the attempt record and the installed
  // backing means the observation model itself is wrong, and hiding it would
  // hide exactly the case this two-source split exists to catch.
  kInconsistentObservation,
  // Not a value; see ZeroCopyPrecondition::kCount.
  kCount,
};
inline constexpr uint32_t kZeroCopyStartupReportCount =
    static_cast<uint32_t>(ZeroCopyStartupReport::kCount);

constexpr bool IsZeroCopyStartupReportValue(ZeroCopyStartupReport value) {
  return static_cast<uint32_t>(value) < kZeroCopyStartupReportCount;
}

constexpr ZeroCopyStartupReport DeriveZeroCopyStartupReport(
    const ZeroCopyObservation& in) {
  // A recorded fact outside the declared vocabulary (kCount itself, or any
  // future enumerator that reached a stale build) describes no run at all. It
  // is rejected here before any rule can read it as a success.
  if (!IsZeroCopyPreconditionValue(in.precondition) ||
      !IsZeroCopyDeviceStepValue(in.device_step)) {
    return ZeroCopyStartupReport::kInconsistentObservation;
  }
  if (!in.requested) {
    // An exact-off run must leave every alias field untouched. Anything else is
    // alias-only work performed without a request.
    if (in.precondition != ZeroCopyPrecondition::kNotEvaluated ||
        in.device_step != ZeroCopyDeviceStep::kNotAttempted ||
        in.alias_buffer_installed) {
      return ZeroCopyStartupReport::kInconsistentObservation;
    }
    return ZeroCopyStartupReport::kNotRequested;
  }
  if (in.precondition == ZeroCopyPrecondition::kNotEvaluated) {
    // Requested but never evaluated: the initializer skipped its own contract.
    return ZeroCopyStartupReport::kInconsistentObservation;
  }
  if (in.precondition != ZeroCopyPrecondition::kSatisfied) {
    if (in.device_step != ZeroCopyDeviceStep::kNotAttempted ||
        in.alias_buffer_installed) {
      return ZeroCopyStartupReport::kInconsistentObservation;
    }
    return ZeroCopyStartupReport::kPreconditionRejected;
  }
  switch (in.device_step) {
    case ZeroCopyDeviceStep::kNotAttempted:
      // Preconditions held and nothing was attempted: unexplained.
      return ZeroCopyStartupReport::kInconsistentObservation;
    case ZeroCopyDeviceStep::kVmRegionRejected:
      return in.alias_buffer_installed
                 ? ZeroCopyStartupReport::kInconsistentObservation
                 : ZeroCopyStartupReport::kVmRegionRejected;
    case ZeroCopyDeviceStep::kAllocationRejected:
      return in.alias_buffer_installed
                 ? ZeroCopyStartupReport::kInconsistentObservation
                 : ZeroCopyStartupReport::kAllocationRejected;
    case ZeroCopyDeviceStep::kContentsRejected:
      return in.alias_buffer_installed
                 ? ZeroCopyStartupReport::kInconsistentObservation
                 : ZeroCopyStartupReport::kContentsIdentityRejected;
    case ZeroCopyDeviceStep::kObserverRejected:
      // Same shape as every other rejection: the alias candidate was released,
      // so an INSTALLED alias here would mean the fallback did not happen and
      // the run is aliasing unobserved -- the one state this step exists to
      // make impossible. It is reported as inconsistent, never as a rejection
      // that quietly kept the alias.
      return in.alias_buffer_installed
                 ? ZeroCopyStartupReport::kInconsistentObservation
                 : ZeroCopyStartupReport::kObserverRejected;
    case ZeroCopyDeviceStep::kAccepted:
      // Accepted by the attempt but the installed backing is not guest RAM:
      // the two sources disagree.
      return in.alias_buffer_installed
                 ? ZeroCopyStartupReport::kAliasActive
                 : ZeroCopyStartupReport::kInconsistentObservation;
    case ZeroCopyDeviceStep::kCount:
      // The sentinel is not a step; unreachable past the guard above, and
      // never a success if it ever is reached.
      return ZeroCopyStartupReport::kInconsistentObservation;
  }
  // No `default:` above, so adding a ZeroCopyDeviceStep enumerator without
  // deciding its report is a compiler warning here and lands on the
  // inconsistent verdict rather than passing silently.
  return ZeroCopyStartupReport::kInconsistentObservation;
}

// Names for the single bounded startup line. Exhaustive switches, no default.
constexpr const char* ZeroCopyPreconditionName(ZeroCopyPrecondition value) {
  switch (value) {
    case ZeroCopyPrecondition::kNotEvaluated:
      return "not_evaluated";
    case ZeroCopyPrecondition::kSatisfied:
      return "satisfied";
    case ZeroCopyPrecondition::kForcedRejection:
      return "forced_rejection";
    case ZeroCopyPrecondition::kNullPointer:
      return "null_pointer";
    case ZeroCopyPrecondition::kEmptyLength:
      return "empty_length";
    case ZeroCopyPrecondition::kInvalidPageSize:
      return "invalid_page_size";
    case ZeroCopyPrecondition::kPointerAlignment:
      return "pointer_alignment";
    case ZeroCopyPrecondition::kLengthAlignment:
      return "length_alignment";
    case ZeroCopyPrecondition::kAddressOverflow:
      return "address_overflow";
    case ZeroCopyPrecondition::kDeviceLimit:
      return "device_limit";
    case ZeroCopyPrecondition::kCount:
      break;  // not a value; falls to the out-of-range bucket
  }
  return "unknown";
}

constexpr const char* ZeroCopyDeviceStepName(ZeroCopyDeviceStep value) {
  switch (value) {
    case ZeroCopyDeviceStep::kNotAttempted:
      return "not_attempted";
    case ZeroCopyDeviceStep::kVmRegionRejected:
      return "vm_region_rejected";
    case ZeroCopyDeviceStep::kAllocationRejected:
      return "allocation_rejected";
    case ZeroCopyDeviceStep::kContentsRejected:
      return "contents_rejected";
    case ZeroCopyDeviceStep::kObserverRejected:
      return "observer_rejected";
    case ZeroCopyDeviceStep::kAccepted:
      return "accepted";
    case ZeroCopyDeviceStep::kCount:
      break;  // not a value; falls to the out-of-range bucket
  }
  return "unknown";
}

constexpr const char* ZeroCopyStartupReportName(ZeroCopyStartupReport value) {
  switch (value) {
    case ZeroCopyStartupReport::kNotRequested:
      return "not_requested";
    case ZeroCopyStartupReport::kPreconditionRejected:
      return "precondition_rejected";
    case ZeroCopyStartupReport::kVmRegionRejected:
      return "vm_region_rejected";
    case ZeroCopyStartupReport::kAllocationRejected:
      return "allocation_rejected";
    case ZeroCopyStartupReport::kContentsIdentityRejected:
      return "contents_identity_rejected";
    case ZeroCopyStartupReport::kObserverRejected:
      return "observer_rejected";
    case ZeroCopyStartupReport::kAliasActive:
      return "alias_active";
    case ZeroCopyStartupReport::kInconsistentObservation:
      return "inconsistent_observation";
    case ZeroCopyStartupReport::kCount:
      break;  // not a value; falls to the out-of-range bucket
  }
  return "unknown";
}

}  // namespace metal
}  // namespace gpu
}  // namespace rex

#endif  // XENIA_GPU_METAL_METAL_ZERO_COPY_CONTRACT_H_
