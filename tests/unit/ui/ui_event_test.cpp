/**
 * @file        tests/unit/ui/ui_event_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ui/ui_event.h>

using rex::ui::UIEvent;

REX_TEST_CASE("As returns the class that carries the type", "[ui][ui_event]") {
  rex::ui::TouchEvent touch(UIEvent::Type::kTouch, 1, rex::ui::TouchEvent::Action::kDown, 2.0f,
                            3.0f);
  const UIEvent& event = touch;
  REQUIRE(event.GetType() == UIEvent::Type::kTouch);
  REQUIRE(event.As<rex::ui::TouchEvent>() == &touch);
  REQUIRE(event.As<rex::ui::UISetupEvent>() == nullptr);
}

REX_TEST_CASE("Resize and focus are setup events", "[ui][ui_event]") {
  rex::ui::UISetupEvent resize(UIEvent::Type::kResize);
  REQUIRE(static_cast<const UIEvent&>(resize).As<rex::ui::UISetupEvent>() == &resize);
  rex::ui::UISetupEvent focus(UIEvent::Type::kFocus, true);
  const rex::ui::UISetupEvent* found =
      static_cast<const UIEvent&>(focus).As<rex::ui::UISetupEvent>();
  REQUIRE(found != nullptr);
  REQUIRE(found->IsInitialSetup());
}

REX_TEST_CASE("A plain event carries no subclass", "[ui][ui_event]") {
  UIEvent close(UIEvent::Type::kClose);
  REQUIRE(close.As<rex::ui::TouchEvent>() == nullptr);
  REQUIRE(close.As<rex::ui::UISetupEvent>() == nullptr);
}
