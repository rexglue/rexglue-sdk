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
  rex::ui::KeyEvent key(UIEvent::Type::kKeyDown, nullptr, rex::hid::VirtualKey::kF3, 1, false,
                        false, false, false);
  const UIEvent& event = key;
  REQUIRE(event.GetType() == UIEvent::Type::kKeyDown);
  REQUIRE(event.As<rex::ui::KeyEvent>() == &key);
  REQUIRE(event.As<rex::ui::MouseEvent>() == nullptr);
}

REX_TEST_CASE("ResizeEvent and FocusEvent are setup events with their data", "[ui][ui_event]") {
  rex::ui::ResizeEvent resize(nullptr, {1280, 720});
  const UIEvent& event = resize;
  REQUIRE(event.As<rex::ui::ResizeEvent>()->GetSize().width_px == 1280);
  REQUIRE(event.As<rex::ui::UISetupEvent>() != nullptr);
  rex::ui::FocusEvent focus(nullptr, false);
  REQUIRE(static_cast<const UIEvent&>(focus).As<rex::ui::FocusEvent>()->IsFocused() == false);
}

REX_TEST_CASE("A plain event carries no subclass", "[ui][ui_event]") {
  UIEvent close(UIEvent::Type::kClose, nullptr);
  REQUIRE(close.As<rex::ui::KeyEvent>() == nullptr);
  REQUIRE(close.As<rex::ui::UISetupEvent>() == nullptr);
}
