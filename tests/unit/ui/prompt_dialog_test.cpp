/**
 * @file        tests/unit/ui/prompt_dialog_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <string>

#include <rex/ui/dialog.h>

#include "ui/prompt.h"

using rex::ui::PromptConfig;
using rex::ui::PromptDialog;
using rex::ui::PromptDone;
using rex::ui::PromptResult;

namespace {

struct Answers {
  int calls = 0;
  PromptResult last;

  PromptDone Done() {
    return PromptDone([this](PromptResult result) {
      ++calls;
      last = std::move(result);
    });
  }
};

}  // namespace

REX_TEST_CASE("a prompt completes once with the pressed button", "[ui][prompt]") {
  Answers answers;
  {
    PromptDialog dialog({.title = "Pick", .buttons = {"No", "Yes"}}, answers.Done());
    dialog.Choose(1, {});
    CHECK(answers.calls == 1);
    CHECK_FALSE(answers.last.canceled);
    CHECK(answers.last.button == 1);
  }
  CHECK(answers.calls == 1);
}

REX_TEST_CASE("a text prompt returns the accepted text", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog({.title = "Name", .text_input = true, .default_text = "abc"}, answers.Done());
  CHECK(dialog.GetText() == "abc");
  dialog.Choose(0, dialog.GetText());
  CHECK(answers.calls == 1);
  CHECK(answers.last.text == "abc");
}

REX_TEST_CASE("a text prompt holds max_length characters", "[ui][prompt]") {
  Answers answers;
  PromptDialog dialog(
      {.title = "Name", .text_input = true, .default_text = "abcdef", .max_length = 3},
      answers.Done());
  CHECK(dialog.GetText() == "abc");
}

REX_TEST_CASE("a prompt destroyed without a button completes cancelled", "[ui][prompt]") {
  Answers answers;
  {
    PromptDialog dialog({.title = "Pick"}, answers.Done());
    CHECK(answers.calls == 0);
  }
  CHECK(answers.calls == 1);
  CHECK(answers.last.canceled);
}
