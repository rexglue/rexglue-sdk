/**
 * @file        ui/sdl3/input_listener_list.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include <rex/types.h>

namespace rex::ui {

class WindowInputListener;

/** What a propagation does after a listener call. */
enum class InputListenerStep : u8 {
  kNext,    /**< Calls the next listener. */
  kStop,    /**< Ends the propagation. */
  kAbandon, /**< Ends the propagation without touching the list, which no longer exists. */
};

/** Input listeners ordered by Z order, safe to edit from a listener during propagation. */
class InputListenerList {
 public:
  /**
   * Adds a listener, or moves it to a new Z order.
   *
   * @param listener The listener.
   * @param z_order The Z order, higher runs first.
   */
  void Add(WindowInputListener* listener, size_t z_order) {
    auto it_existing = Find(listener);
    if (it_existing != listeners_.end()) {
      if (it_existing->first == z_order) {
        return;
      }
      Erase(it_existing);
    }
    auto it_new = listeners_.emplace_hint(listeners_.lower_bound(z_order), z_order, listener);
    for (IterationContext* context = innermost_; context; context = context->outer_context) {
      if (z_order < context->current_z_order && (context->next_iterator == listeners_.cend() ||
                                                 z_order >= context->next_iterator->first)) {
        context->next_iterator = it_new;
      }
    }
  }

  /**
   * Removes a listener.
   *
   * @param listener The listener.
   */
  void Remove(WindowInputListener* listener) {
    auto it_existing = Find(listener);
    if (it_existing != listeners_.end()) {
      Erase(it_existing);
    }
  }

  /**
   * Calls each listener once, from the highest Z order down.
   *
   * @tparam F The callback type.
   * @param fn Called with each listener, returns the step to take next.
   * @return False when fn abandoned the list.
   */
  template <std::invocable<WindowInputListener*> F>
  bool Propagate(F&& fn) {
    IterationContext context(innermost_, listeners_.cbegin());
    innermost_ = &context;
    while (context.next_iterator != listeners_.cend()) {
      context.current_z_order = context.next_iterator->first;
      WindowInputListener* listener = (context.next_iterator++)->second;
      if (std::ranges::contains(context.called, listener)) {
        continue;
      }
      if (context.called.empty()) {
        context.called.reserve(listeners_.size());
      }
      context.called.push_back(listener);
      const InputListenerStep step = fn(listener);
      if (step == InputListenerStep::kAbandon) {
        return false;
      }
      if (step == InputListenerStep::kStop) {
        break;
      }
    }
    innermost_ = context.outer_context;
    return true;
  }

  /**
   * Reports whether a propagation is running.
   *
   * @return True while a propagation is running.
   */
  bool GetPropagating() const { return innermost_ != nullptr; }

 private:
  using Map = std::multimap<size_t, WindowInputListener*, std::greater<>>;

  struct IterationContext {
    IterationContext(IterationContext* outer, const Map::const_iterator& first)
        : outer_context(outer), next_iterator(first) {}
    IterationContext* outer_context;
    Map::const_iterator next_iterator;
    size_t current_z_order = SIZE_MAX;
    std::vector<WindowInputListener*> called;
  };

  Map::iterator Find(WindowInputListener* listener) {
    return std::ranges::find_if(listeners_,
                                [listener](const auto& entry) { return entry.second == listener; });
  }

  void Erase(const Map::iterator& erased) {
    for (IterationContext* context = innermost_; context; context = context->outer_context) {
      if (context->next_iterator == erased) {
        ++context->next_iterator;
      }
    }
    listeners_.erase(erased);
  }

  Map listeners_;
  IterationContext* innermost_ = nullptr;
};

}  // namespace rex::ui
