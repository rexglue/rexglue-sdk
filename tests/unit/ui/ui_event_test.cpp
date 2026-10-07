/**
 * @file        tests/unit/ui/ui_event_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/ui/ui_event.h>

using rex::ui::UIEvent;

TEST_CASE("As returns the class that carries the type", "[ui_event]") {
  rex::ui::KeyEvent key(UIEvent::Type::KeyDown, nullptr, rex::hid::VirtualKey::kF3, 1, false, false,
                        false, false);
  const UIEvent& event = key;
  REQUIRE(event.type() == UIEvent::Type::KeyDown);
  REQUIRE(event.As<rex::ui::KeyEvent>() == &key);
  REQUIRE(event.As<rex::ui::MouseEvent>() == nullptr);
}

TEST_CASE("ResizeEvent and FocusEvent are setup events with their data", "[ui_event]") {
  rex::ui::ResizeEvent resize(nullptr, {1280, 720});
  const UIEvent& event = resize;
  REQUIRE(event.As<rex::ui::ResizeEvent>()->size().width_px == 1280);
  REQUIRE(event.As<rex::ui::UISetupEvent>() != nullptr);
  rex::ui::FocusEvent focus(nullptr, false);
  REQUIRE(static_cast<const UIEvent&>(focus).As<rex::ui::FocusEvent>()->focused() == false);
}

TEST_CASE("A plain event carries no subclass", "[ui_event]") {
  UIEvent close(UIEvent::Type::Close, nullptr);
  REQUIRE(close.As<rex::ui::KeyEvent>() == nullptr);
  REQUIRE(close.As<rex::ui::UISetupEvent>() == nullptr);
}
