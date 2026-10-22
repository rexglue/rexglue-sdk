/**
 * @file        tests/unit/ppc/midasm_macro_fail/accepts_attachment.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/midasm.h>

REX_MIDASM_BRANCH(AcceptedBody, (u32 size, PPCRegister& held)) {
  return size == 0 && held.u64 == 0;
}
REX_MIDASM_AT(AcceptedBody, 0x82000100, before, 0x8200010C, r5, r31);
