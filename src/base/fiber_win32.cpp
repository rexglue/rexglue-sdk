/**
 * @file        base/fiber_win32.cpp
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
#include <rex/platform/thread.h>

#include <cassert>
#include <memory>

namespace rex::platform {

namespace {
thread_local Fiber* g_current_fiber = nullptr;
}  // namespace

Fiber* Fiber::GetCurrent() {
  return g_current_fiber;
}

Fiber* Fiber::ConvertCurrentThread() {
  auto f = std::make_unique<Fiber>();
  f->handle_ = ::ConvertThreadToFiber(nullptr);
  if (!f->handle_) {
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  g_current_fiber = f.get();
  return f.release();
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto f = std::make_unique<Fiber>();
  f->handle_ = ::CreateFiber(stack_size, reinterpret_cast<LPFIBER_START_ROUTINE>(entry), arg);
  if (!f->handle_) {
    return nullptr;
  }
  return f.release();
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
