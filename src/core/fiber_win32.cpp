/**
 * @file        rex/core/fiber_win32.cpp
 * @brief       Win32 backend for rex::thread::Fiber
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WIN32

#include <rex/platform/win.h>

#include <rex/thread/fiber.h>

#include <cassert>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
    auto* f    = new Fiber();
    f->handle_ = ::ConvertThreadToFiber(nullptr);
    if (!f->handle_) {
        delete f;
        return nullptr;
    }
    f->is_thread_fiber_ = true;
    tls_current_        = f;
    return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
    auto* f    = new Fiber();
    f->handle_ = ::CreateFiber(stack_size,
        reinterpret_cast<LPFIBER_START_ROUTINE>(entry), arg);
    if (!f->handle_) {
        delete f;
        return nullptr;
    }
    return f;
}

void Fiber::SwitchTo(Fiber* target) {
    tls_current_ = target;
    ::SwitchToFiber(target->handle_);
}

void Fiber::Destroy() {
    // Thread fibers are destroyed from the owning thread itself.
    if (is_thread_fiber_) {
        ::ConvertFiberToThread();
        tls_current_ = nullptr;
    } else {
        assert(this != tls_current_ && "Destroy called on the currently running fiber");
        ::DeleteFiber(handle_);
    }
    delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WIN32
