/**
 * @file        tests/unit/ppc/midasm_macro_fail/count_mismatch.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/midasm.h>

REX_MIDASM_HOOK(CountBody, (u32 first, u32 second)) {
  (void)first;
  (void)second;
}
REX_MIDASM_AT(CountBody, 0x82000100, after, r3);
