/**
 * @file        tests/unit/ppc/midasm_macro_fail/bad_address.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ppc/midasm.h>

REX_MIDASM_HOOK(AddressBody, (u32 value)) {
  (void)value;
}
REX_MIDASM_AT(AddressBody, 0x82000102, after, r3);
