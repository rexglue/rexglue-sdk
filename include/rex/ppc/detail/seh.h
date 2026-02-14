/**
 * @file        ppc/detail/seh.h
 * @brief       Platform-specific SEH implementation details
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#pragma once

#include <atomic>
#include <cstdint>

namespace rex {
class SehException;
}

namespace rex::ppc::detail {

inline std::atomic<bool> g_seh_initialized{false};

/// Thread-local SEH state for capturing exception info.
struct SehThreadState {
  uint32_t code = 0;
  uintptr_t info[2] = {0, 0};
};

/// Get the thread-local SEH state.
SehThreadState& seh_thread_state();

/// SEH filter function - captures exception info and determines whether to handle.
/// Returns non-zero if the exception should be handled.
int seh_filter(uint32_t code, void* exception_pointers);

/// Re-raise the captured exception.
[[noreturn]] void seh_rethrow();

/// Initialize SEH signal handlers (POSIX only, no-op on Windows).
void seh_initialize();

/// Thread-local flag for POSIX: true when in SEH-protected code.
bool& seh_active();

}  // namespace rex::ppc::detail
