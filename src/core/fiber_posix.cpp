/**
 * @file        rex/core/fiber_posix.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin exposes ucontext only with a feature level set before <ucontext.h>.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/platform/fiber.h>

#include <cassert>
#include <memory>

namespace rex::platform {

namespace {
thread_local Fiber* g_current_fiber = nullptr;
}  // namespace

Fiber* Fiber::Current() {
  return g_current_fiber;
}

Fiber* Fiber::ConvertCurrentThread() {
  auto f = std::make_unique<Fiber>();
  if (getcontext(&f->context_) == -1) {
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  g_current_fiber = f.get();
  return f.release();
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto f = std::make_unique<Fiber>();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f.release();
}

/*static*/ void Fiber::Trampoline() {
  Fiber* f = g_current_fiber;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = g_current_fiber;
  g_current_fiber = target;
  swapcontext(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    g_current_fiber = nullptr;
  } else {
    assert(this != g_current_fiber && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::platform

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
