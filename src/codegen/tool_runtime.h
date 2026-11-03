/**
 * @file        codegen/tool_runtime.h
 * @brief       A loaded TitleRuntime for the tools that read a title and never run it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/app/app.h>
#include <rex/result.h>
#include <rex/types.h>
#include <rex/utility.h>

namespace rex {
namespace kernel {
class TitleRuntime;
}  // namespace kernel
}  // namespace rex

namespace rex::codegen {

class ToolApp;

/** Owns a loaded TitleRuntime that is never launched. */
class ToolRuntime : public MoveOnly {
 public:
  ToolRuntime(ToolRuntime&& other) noexcept;
  ToolRuntime& operator=(ToolRuntime&& other) noexcept;
  ~ToolRuntime();

  [[nodiscard]] rex::kernel::TitleRuntime& GetRuntime() const { return *runtime_; }

 private:
  friend class ToolApp;

  explicit ToolRuntime(std::unique_ptr<rex::kernel::TitleRuntime> runtime);

  std::unique_ptr<rex::kernel::TitleRuntime> runtime_;
};

/** An app that selects no driver and opens tool runtimes. It starts on the first Open. */
class ToolApp : public rex::app::App {
 public:
  /**
   * Creates the app. Nothing starts until Open.
   *
   * @param app_root The app root, empty for the default under the user profile.
   */
  explicit ToolApp(std::string_view app_name, std::filesystem::path app_root = {});
  ~ToolApp() override;

  /**
   * Starts the app when it has not started. Open calls it, and a command that needs the modules
   * without a title calls it first.
   *
   * @return Success, or the reason the app cannot start.
   */
  [[nodiscard]] Result<void> Start();

  /**
   * Loads a title from a game folder without running it.
   *
   * @param game_root The host folder mapped to the game device.
   * @param entry The executable on the game device that Load runs, empty loads none.
   * @param imports Guest paths the executable may import from, with backslashes.
   * @return The loaded runtime, or the error that stopped the start or the load.
   */
  [[nodiscard]] Result<ToolRuntime> Open(const std::filesystem::path& game_root,
                                         std::string_view entry = {},
                                         std::span<const std::string> imports = {});

 protected:
  Result<void> OnConfigure(Settings& settings) override;

 private:
  std::filesystem::path app_root_;
  bool started_ = false;
};

}  // namespace rex::codegen
