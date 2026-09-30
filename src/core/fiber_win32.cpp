/**
 * @file        rex/core/fiber_win32.cpp
 * @brief       Win32 backend for rex::platform::Fiber
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WIN32
#include <Windows.h>
#include <rex/platform/fiber.h>

#include <cassert>

namespace rex::platform {

namespace {
thread_local Fiber* g_current_fiber = nullptr;
}  // namespace

Fiber* Fiber::Current() {
  return g_current_fiber;
}

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->handle_ = ::ConvertThreadToFiber(nullptr);
  if (!f->handle_) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  g_current_fiber = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->handle_ = ::CreateFiber(stack_size, reinterpret_cast<LPFIBER_START_ROUTINE>(entry), arg);
  if (!f->handle_) {
    delete f;
    return nullptr;
  }
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  g_current_fiber = target;
  ::SwitchToFiber(target->handle_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    ::ConvertFiberToThread();
    g_current_fiber = nullptr;
  } else {
    assert(this != g_current_fiber && "Destroy called on the currently running fiber");
    ::DeleteFiber(handle_);
  }
  delete this;
}

}  // namespace rex::platform

#endif  // REX_PLATFORM_WIN32
