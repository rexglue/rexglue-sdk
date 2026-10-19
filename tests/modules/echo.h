/**
 * @file        tests/modules/echo.h
 * @brief       Test interface served by the good and duplicate-echo test modules
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/module.h>

namespace rex::test {

/**
 * Echoes an integer. Served by the test modules.
 */
REX_INTERFACE(IEcho, "rex.test.echo", 1)
 public:
  /**
   * Returns its argument.
   *
   * @param value The value to return.
   * @return The value passed in.
   */
  virtual int Echo(int value) = 0;

  /**
   * Returns the echoed value cvar.
   *
   * @return The current value of the echo.value cvar the module registers at Connect.
   */
  virtual int GetValue() = 0;
};

}  // namespace rex::test
