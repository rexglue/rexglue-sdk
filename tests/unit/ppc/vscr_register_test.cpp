/**
 * @file        tests/unit/ppc/vscr_register_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/context.h>

REX_TEST_CASE("VSCR Read and Write round trip the saturation and non Java bits", "[ppc][vscr]") {
  rex::ppc::VSCRRegister vscr;
  CHECK(vscr.Read() == 0);
  vscr.Write(0x00010001);
  CHECK(vscr.sat == 1);
  CHECK(vscr.nj == 1);
  CHECK(vscr.Read() == 0x00010001);
  vscr.Write(0x00010000);
  CHECK(vscr.sat == 0);
  CHECK(vscr.nj == 1);
  CHECK(vscr.Read() == 0x00010000);
  vscr.Write(0x00000001);
  CHECK(vscr.sat == 1);
  CHECK(vscr.nj == 0);
  CHECK(vscr.Read() == 0x00000001);
}

REX_TEST_CASE("VSCR Write ignores every other bit", "[ppc][vscr]") {
  rex::ppc::VSCRRegister vscr;
  vscr.Write(0xFFFEFFFE);
  CHECK(vscr.Read() == 0);
  vscr.Write(0xFFFFFFFF);
  CHECK(vscr.Read() == 0x00010001);
}
