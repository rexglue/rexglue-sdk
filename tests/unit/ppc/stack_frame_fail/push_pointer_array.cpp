/**
 * @file        tests/unit/ppc/stack_frame_fail/push_pointer_array.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>

#include <rex/ppc/stack.h>

void PushPointerArray(rex::ppc::StackFrame& frame, const std::array<u32*, 2>& value) {
  frame.Push(value);
}
