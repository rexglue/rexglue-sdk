/**
 * @file        command.h
 * @brief       Console commands an owner registers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/result.h>
#include <rex/setting.h>
#include <rex/utility.h>

namespace rex {

/** A console command an owner registers, declared with REX_COMMAND. Runs on the calling thread. */
class REX_API Command : public NonCopyable {
 public:
  /**
   * @param run Called with the argument text.
   */
  Command(SettingOwner* owner, std::string_view name, std::string_view help,
          MoveOnlyFunction<Result<void>(std::string_view args)> run);

  ~Command();

  /**
   * Returns the full name.
   */
  const std::string& GetName() const;

  const std::string& GetHelp() const;

  [[nodiscard]] Result<void> Run(std::string_view args);

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace rex

/**
 * Declares a console command inside a SettingOwner.
 *
 * @param name The member name.
 *
 * @code
 * REX_COMMAND(clear, "Clears the log", [](std::string_view) { return Ok(); });
 * @endcode
 *
 * @param fn Called with the argument text, returns Result<void>.
 */
#define REX_COMMAND(name, help, fn) \
  ::rex::Command name {             \
    this, #name, help, fn           \
  }
