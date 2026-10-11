/**
 * @file        tests/unit/gpu/packet_header_test.cpp
 * @brief       PM4 packet headers decode with the command processor field layout
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/types.h>
#include <rex/xenos/xenos.h>

REX_TEST_CASE("BuildPacketType0 encodes the 1 register flag", "[gpu]") {
  for (bool one_reg : {false, true}) {
    u32 packet = rex::xenos::BuildPacketType0(0x4800, 6, one_reg);
    CHECK((packet >> 30) == 0);
    CHECK(((packet >> 16) & 0x3FFF) + 1 == 6);
    CHECK(((packet >> 15) & 0x1) == (one_reg ? 1u : 0u));
    CHECK((packet & 0x7FFF) == 0x4800);
  }
}
