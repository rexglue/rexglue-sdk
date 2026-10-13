/**
 * @file        tests/unit/ui/input_listener_list_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <map>
#include <vector>

#include "ui/sdl3/input_listener_list.h"
#include "ui/sdl3/window_listener.h"

using rex::ui::InputListenerList;
using rex::ui::InputListenerStep;
using rex::ui::WindowInputListener;

REX_TEST_CASE("A listener that lowers its own Z order below a later layer runs once",
              "[ui][input_listener_list]") {
  WindowInputListener first;
  WindowInputListener second;
  InputListenerList list;
  list.Add(&first, 10);
  list.Add(&second, 5);
  std::map<WindowInputListener*, int> calls;
  CHECK(list.Propagate([&](WindowInputListener* listener) {
    ++calls[listener];
    if (listener == &first) {
      list.Add(&first, 0);
    }
    return InputListenerStep::kNext;
  }));
  CHECK(calls[&first] == 1);
  CHECK(calls[&second] == 1);
}

REX_TEST_CASE("A listener moved below the current layer before its turn still runs",
              "[ui][input_listener_list]") {
  WindowInputListener first;
  WindowInputListener second;
  InputListenerList list;
  list.Add(&first, 10);
  list.Add(&second, 5);
  std::map<WindowInputListener*, int> calls;
  CHECK(list.Propagate([&](WindowInputListener* listener) {
    ++calls[listener];
    if (listener == &first) {
      list.Add(&second, 1);
    }
    return InputListenerStep::kNext;
  }));
  CHECK(calls[&first] == 1);
  CHECK(calls[&second] == 1);
}

REX_TEST_CASE("A listener that removes itself leaves the next layer to run",
              "[ui][input_listener_list]") {
  WindowInputListener first;
  WindowInputListener second;
  InputListenerList list;
  list.Add(&first, 10);
  list.Add(&second, 5);
  std::map<WindowInputListener*, int> calls;
  CHECK(list.Propagate([&](WindowInputListener* listener) {
    ++calls[listener];
    if (listener == &first) {
      list.Remove(&first);
    }
    return InputListenerStep::kNext;
  }));
  CHECK(calls[&first] == 1);
  CHECK(calls[&second] == 1);
}

REX_TEST_CASE("Listeners of equal Z order run newest first", "[ui][input_listener_list]") {
  WindowInputListener older;
  WindowInputListener newer;
  InputListenerList list;
  list.Add(&older, 5);
  list.Add(&newer, 5);
  std::vector<WindowInputListener*> order;
  CHECK(list.Propagate([&](WindowInputListener* listener) {
    order.push_back(listener);
    return InputListenerStep::kNext;
  }));
  CHECK(order == std::vector<WindowInputListener*>{&newer, &older});
}
