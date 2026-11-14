/**
 * @file        tests/unit/ppc/midasm_macro_fail/branch_without_target.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/midasm.h>

REX_MIDASM_BRANCH(BranchBody, (u32 value)) {
  return value == 0;
}
REX_MIDASM_AT(BranchBody, 0x82000100, before, r3);
