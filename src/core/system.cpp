/**
 * @file        core/system.cpp
 * @brief       Platform-independent state behind rex/system.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <mutex>
#include <string>

#include <rex/system.h>

namespace rex {

namespace {

struct MessageBoxTitleState {
  std::mutex mutex;
  std::string title;
};

// Message boxes can come from any thread, including another translation
// unit's static initializer, so the state is built on first use and never
// destroyed.
MessageBoxTitleState& TitleState() {
  static MessageBoxTitleState* state = new MessageBoxTitleState();
  return *state;
}

}  // namespace

void SetMessageBoxTitle(std::string_view title) {
  auto& state = TitleState();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.title.assign(title);
}

std::string GetMessageBoxTitle() {
  auto& state = TitleState();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.title;
}

}  // namespace rex
