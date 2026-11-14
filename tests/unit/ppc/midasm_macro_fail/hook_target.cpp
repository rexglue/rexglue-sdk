/**
 * @file        tests/unit/ppc/midasm_macro_fail/hook_target.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/midasm.h>

REX_MIDASM_HOOK(TargetBody, (u32 value)) {
  (void)value;
}
REX_MIDASM_AT(TargetBody, 0x82000100, after, 0x8200010C, r3);
