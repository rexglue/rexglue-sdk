/**
 * @file        tests/samples/app_main.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/app.h>

class FooTool : public rex::App {
 protected:
  rex::i32 OnRun(rex::API&) override { return 0; }
};

rex::i32 FooToolMain(FooTool& tool, const rex::CLIArgList& args) {
  if (tool.Init(args)) {
    tool.Run();
  }
  return tool.Shutdown();
}

REX_APP_MAIN(FooTool, FooToolMain)
