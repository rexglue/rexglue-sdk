/**
 ******************************************************************************
 * Upstream (macXenos) API that this SDK does not have.
 ******************************************************************************
 * Every name here exists because the ported Metal backend calls it and this SDK has no equivalent. Each one is either
 * the smallest honest implementation or a documented placeholder with the runtime consequence written down, so nobody
 * later mistakes it for finished work.
 *
 * Included from metal_upstream_compat.h, which every ported translation unit force-includes.
 */
#pragma once

#include <cstdint>

namespace rex {
namespace threading {

// Upstream brackets a critical section with these so the guest CPU cannot be suspended mid-publication. This runtime
// runs the guest on one thread at a time, so there is nothing to suspend; they are paired bookkeeping hooks.
// If the guest ever gains concurrent threads, these must actually suspend it.
inline void BeginSuspendDeferral() {}
inline void EndSuspendDeferral() {}

}  // namespace threading
}  // namespace rex
