/**
 * @file        tests/modules/echo.h
 * @brief       Test interface served by the good and dupecho test modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/macros.h>

namespace rex::test {

/**
 * Echoes an integer. Served as "rex.test.echo" version 1.
 */
class IEcho {
 public:
  REX_INTERFACE(IEcho, "rex.test.echo", 1);
  virtual ~IEcho() = default;

  /**
   * @return The value passed in.
   */
  virtual int Echo(int value) = 0;
};

}  // namespace rex::test
