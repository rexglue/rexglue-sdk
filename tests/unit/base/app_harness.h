/**
 * @file        tests/unit/base/app_harness.h
 * @brief       App that runs a test body inside Init and Shutdown
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "object_app.h"
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <rex/app/app.h>
#include <rex/cli.h>
#include <rex/setting.h>
#include <rex/settings_registry.h>

#include "temp_folder.h"

namespace rex::test {

/**
 * An app with no providers. Init and Shutdown run the registry steps, such as freezing the startup
 * settings, and the test body runs before and after them.
 */
class HarnessApp final : public rex::test::ObjectApp<rex::app::App> {
 public:
  /** Runs in OnConfigure, before the layers apply. */
  std::function<Result<void>(SettingsRegistry&)> configure;
  /** Runs in OnInit, after the startup settings froze. */
  std::function<Result<void>()> init;
  std::filesystem::path folder;
  /** What the test body threw, rethrown by Finish. */
  std::exception_ptr failure;

  explicit HarnessApp(std::filesystem::path given = {}) : ObjectApp("harness") {
    if (given.empty()) {
      owned_ = std::make_unique<TempFolder>("rex_harness_");
      folder = owned_->path;
    } else {
      folder = std::move(given);
    }
  }

  Result<void> Start(const std::vector<std::string>& args = {}) {
    return Init(CLIArgList::Parse(args));
  }

  /** Runs Shutdown, then rethrows what the test body threw. */
  int Finish() {
    const int code = Shutdown();
    if (failure) {
      std::rethrow_exception(failure);
    }
    return code;
  }

 protected:
  Result<void> OnConfigure(SettingsRegistry& settings) override {
    REX_TRY_VOID(root.SetText(SettingLayer::kTitle, folder.string()));
    return Guard([&] { return configure ? configure(settings) : Ok(); });
  }

  Result<void> OnInit() override {
    return Guard([&] { return init ? init() : Ok(); });
  }

 private:
  template <typename Fn>
  Result<void> Guard(Fn&& fn) {
    try {
      return fn();
    } catch (...) {
      failure = std::current_exception();
      return Err(ErrorCategory::kRuntime, "the test body threw");
    }
  }

  std::unique_ptr<TempFolder> owned_;
};

/**
 * Runs a body in OnInit of a fresh app and ends the session.
 *
 * @param args The command line.
 * @return The exit code.
 */
inline int RunInInit(std::function<void()> body, const std::vector<std::string>& args = {}) {
  HarnessApp app;
  app.init = [&body]() -> Result<void> {
    body();
    return Ok();
  };
  const bool started = static_cast<bool>(app.Start(args));
  const int code = app.Finish();
  if (!started) {
    throw std::runtime_error("the harness app failed to start");
  }
  return code;
}

}  // namespace rex::test
