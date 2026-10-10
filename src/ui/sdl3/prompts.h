/**
 * @file        ui/sdl3/prompts.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <concepts>
#include <string>
#include <type_traits>
#include <vector>

#include <rex/types.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/user_interface.h>
#include <rex/move_only_function.h>

namespace rex::ui {

class PromptDone : public rex::MoveOnly {
 public:
  PromptDone() = default;

  template <typename F>
    requires(!std::same_as<std::remove_cvref_t<F>, PromptDone> && std::invocable<F&, PromptResult>)
  PromptDone(F&& done) : done_(std::forward<F>(done)) {}

  PromptDone(PromptDone&& other) noexcept : done_(std::move(other.done_)) { other.done_ = nullptr; }

  PromptDone& operator=(PromptDone&& other) noexcept {
    if (this != &other) {
      (*this)(PromptResult{});
      done_ = std::move(other.done_);
      other.done_ = nullptr;
    }
    return *this;
  }

  ~PromptDone() { (*this)(PromptResult{}); }

  void operator()(PromptResult result) {
    if (!done_) {
      return;
    }
    auto done = std::move(done_);
    done_ = nullptr;
    done(std::move(result));
  }

  explicit operator bool() const { return static_cast<bool>(done_); }

 private:
  rex::MoveOnlyFunction<void(PromptResult)> done_;
};

class PromptDialog final : public ImGuiDialog {
 public:
  PromptDialog(PromptDesc desc, PromptDone done);
  ~PromptDialog() override;

  void Choose(u32 button, std::string text);
  std::string Text() const;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Complete(PromptResult result);
  void DrawButtons();
  void DrawInput();

  PromptDesc desc_;
  PromptDone done_;
  std::vector<std::string> buttons_;
  std::vector<char> buffer_;
  std::string label_;
  bool opened_ = false;
};

}  // namespace rex::ui
