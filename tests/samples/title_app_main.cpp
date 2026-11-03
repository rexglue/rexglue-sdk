/**
 * @file        tests/samples/title_app_main.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/app/title_app.h>

rex::ppc::PPCImage PPCImageConfig{};

class FooApp : public rex::app::TitleApp {
 public:
  FooApp() : TitleApp("foo", PPCImageConfig) {}
};

int FooMain(FooApp& app, const rex::CLIArgList& args) {
  if (app.Init(args)) {
    for (int frame = 0; frame < 3 && app.RunFrame(); ++frame) {}
    if (app.LaunchTitle()) {
      app.Run();
    }
  }
  return app.Shutdown();
}

REX_TITLE_APP_MAIN(FooApp, FooMain)
