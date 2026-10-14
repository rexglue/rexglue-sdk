/**
 * @file        platform/fiber.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>
#include <rex/platform.h>
#include <rex/types.h>
#include <cstddef>

#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#if REX_PLATFORM_MAC && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>
#include <cstdint>
#include <vector>
#endif

namespace rex::platform {

/**
 * Host OS fiber. Each guest fiber owns 1 Fiber. A switch keeps the whole C++ call stack, so a
 * function resumes mid body without a link register lookup.
 */
struct REX_API Fiber {
  /**
   * Converts the calling thread into a fiber. Call once per thread before any SwitchTo.
   *
   * @return Fiber for the current execution context, or null on failure.
   */
  static Fiber* ConvertCurrentThread();

  /**
   * Creates a fiber with its own host stack. The first switch to it calls entry(arg).
   *
   * @param stack_size Stack size in bytes.
   * @param entry Entry function.
   * @param arg Argument passed to entry.
   * @return The new fiber, or null on failure.
   */
  static Fiber* Create(size_t stack_size, void (*entry)(void*), void* arg);

  /**
   * Suspends the current fiber and resumes target. Returns when another fiber switches back.
   *
   * @param target Fiber to resume.
   */
  static void SwitchTo(Fiber* target);

  /**
   * Destroys and frees this fiber. A created fiber must not be running. A converted thread
   * fiber is destroyed from its own thread and turns the thread back into a plain thread.
   */
  void Destroy();

  /**
   * Returns the fiber running on the calling thread.
   *
   * @return The current fiber, or null when the thread was never converted.
   */
  static Fiber* GetCurrent();

 private:
#if REX_PLATFORM_WIN32
  void* handle_ = nullptr;
  bool is_thread_fiber_ = false;
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  ucontext_t context_{};
  std::vector<u8> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  static void Trampoline();
#endif
};

}  // namespace rex::platform
