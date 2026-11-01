/**
 * @file        tests/unit/ppc/stack_frame_fail/push_mapped_ptr_array.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>

#include <rex/memory/mapped_ptr.h>
#include <rex/ppc/stack.h>

void PushMappedPtrArray(rex::ppc::StackFrame& frame,
                        const std::array<rex::memory::MappedPtr<u32>, 2>& value) {
  frame.Push(value);
}
