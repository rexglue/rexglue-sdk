/**
 * @file        tests/samples/title_app.cpp
 * @brief       Compile check for REX_TITLE_APP
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/title_app.h>

rex::PPCImageInfo PPCImageConfig{};

class FooApp : public rex::ui::TitleApp {
 protected:
  void OnCreateWindow(rex::ui::TitleUI& ui) override {
    if (!ui.overlays().Add(rex::ui::kStandardOverlays)) {
      rex::RequestShutdown(1);
    }
  }
};

REX_TITLE_APP(FooApp)
