/**
 * @file        tests/unit/ppc/stack_frame_fail/push_pointer.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/stack.h>

void PushPointer(rex::ppc::StackFrame& frame, u32* value) {
  frame.Push(value);
}
