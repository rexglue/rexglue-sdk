/**
 * @file        tests/samples/console_app.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/app/app.h>

class FooTool : public rex::app::App {
 public:
  FooTool() : App("foo") {}

 protected:
  void OnRun() override {}
};

REX_CONSOLE_APP(FooTool)
