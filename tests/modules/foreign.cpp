/**
 * @file        tests/modules/foreign.cpp
 * @brief       Test module that exports no build identity
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

extern "C" REX_DETAIL_EXPORT int ReXForeignMarker() {
  return 0;
}
