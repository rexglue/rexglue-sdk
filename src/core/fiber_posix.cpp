/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>
#include <ucontext.h>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
    auto* f = new Fiber();
    if (getcontext(&f->context_) == -1) {
        delete f;
        return nullptr;
    }
    f->is_thread_fiber_ = true;
    tls_current_        = f;
    return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
    auto* f   = new Fiber();
    f->entry_ = entry;
    f->arg_   = arg;
    f->stack_.resize(stack_size);

    if (getcontext(&f->context_) == -1) {
        delete f;
        return nullptr;
    }
    f->context_.uc_stack.ss_sp   = f->stack_.data();
    f->context_.uc_stack.ss_size = f->stack_.size();
    f->context_.uc_link          = nullptr;
    // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
    makecontext(&f->context_, &Fiber::Trampoline, 0);
    return f;
}

/*static*/ void Fiber::Trampoline() {
    // tls_current_ was updated by SwitchTo before swapcontext returned here.
    Fiber* f = tls_current_;
    f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
    Fiber* from  = tls_current_;
    tls_current_ = target;
    swapcontext(&from->context_, &target->context_);
}

void Fiber::Destroy() {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
    // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
    delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
