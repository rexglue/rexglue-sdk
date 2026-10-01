/**
 * @file        codegen/tool_runtime.h
 * @brief       A tool mode Runtime over the mount table rexfilesystem serves
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

#include <rex/result.h>
#include <rex/types.h>

namespace rex {
class API;
class Runtime;
}  // namespace rex

namespace rex::codegen {

/**
 * Owns a tool mode Runtime and the rex::Init that serves its mount table. The destructor shuts
 * the Runtime down and then calls rex::Shutdown. Move only.
 */
class ToolRuntime : public MoveOnly {
 public:
  ToolRuntime(ToolRuntime&& other) noexcept;
  ToolRuntime& operator=(ToolRuntime&& other) noexcept;
  ~ToolRuntime();

  /**
   * Returns the Runtime.
   *
   * @return The Runtime, set up with game colon at the game root.
   */
  [[nodiscard]] Runtime& runtime() const { return *runtime_; }

  /**
   * Returns the API that serves the runtime's interfaces.
   *
   * @return The API, valid until the ToolRuntime is destroyed.
   */
  API& api() const { return *api_; }

 private:
  friend Result<ToolRuntime> OpenToolRuntime(const std::filesystem::path& game_root);

  ToolRuntime(std::unique_ptr<Runtime> runtime, API* api);
  void Release();

  std::unique_ptr<Runtime> runtime_;
  API* api_ = nullptr;
};

/**
 * Runs rex::Init with the rexfilesystem module and builds a tool mode Runtime whose game colon
 * mount is game_root.
 *
 * @param game_root The host folder that game colon names.
 * @return The runtime, or the rex::Init or Runtime::Setup error.
 */
[[nodiscard]] Result<ToolRuntime> OpenToolRuntime(const std::filesystem::path& game_root);

}  // namespace rex::codegen
