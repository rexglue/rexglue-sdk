/**
 * @file        tests/unit/ppc/stack_frame_fail/push_mapped_ptr.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/stack.h>

void PushMappedPtr(rex::ppc::StackFrame& frame, rex::MappedPtr<u32> value) {
  frame.Push(value);
}
