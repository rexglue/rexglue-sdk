/**
 * @file        tests/unit/ui/imgui_dialog_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <imgui.h>

#include <rex/types.h>
#include <rex/ui/ui_system.h>
#include <rex/utility.h>

namespace {

class CountingDialog final : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO&) override { return true; }
};

class IdleDialog final : public rex::ui::DialogBase {
 public:
  bool idle = false;

  bool Draw(ImGuiIO&) override { return true; }

  bool NeedsContinuousPaint() const override { return !idle; }
};

}  // namespace

REX_TEST_CASE("A dialog needs continuous paint unless it says otherwise", "[ui][imgui]") {
  CountingDialog counting;
  CHECK(counting.NeedsContinuousPaint());
  CHECK_FALSE(counting.WantsExclusiveInput());
  CHECK_FALSE(counting.OnToggle());
  IdleDialog idle;
  CHECK(idle.NeedsContinuousPaint());
  idle.idle = true;
  CHECK_FALSE(idle.NeedsContinuousPaint());
}
