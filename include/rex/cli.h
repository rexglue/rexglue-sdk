/**
 * @file        cli.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex {

struct CLIOption {
  /** Name without the leading dashes. */
  std::string name;
  /** Value of --name=value or --name value, empty when the option has none. */
  std::optional<std::string> value;
};

/**
 * The command line, split into options and positional arguments. An option is --name or
 * --name=value, and a token after --name that does not start with -- is its value.
 */
class REX_API CLIArgList {
 public:
  /**
   * Parses the command line main receives.
   *
   * @param argc Argument count, including the program name.
   * @param argv Argument vector. argv[0] is the program name and is skipped.
   */
  static CLIArgList Parse(i32 argc, char** argv);

  /**
   * Parses command line arguments held as strings.
   *
   * @param args Command line arguments without the program name.
   */
  static CLIArgList Parse(std::span<const std::string> args);

  /**
   * Reports whether an option is present.
   *
   * @param name Option name without the leading dashes.
   * @return True when --name or --name=value is present.
   */
  bool Has(std::string_view name) const;

  /**
   * Reads the value of an option.
   *
   * @param name Option name without the leading dashes.
   * @return The value given as --name=value or --name value, or std::nullopt when the option
   *         is absent or has no value.
   */
  std::optional<std::string_view> Get(std::string_view name) const;

  /** Returns the options in the order given. Each option has a name and a value if it has one. */
  std::span<const CLIOption> GetOptions() const;

  /** Returns the arguments that are not options or option values. */
  std::span<const std::string> GetPositionals() const;

  /**
   * @return The arguments in their original order, without the program name.
   */
  std::span<const std::string> GetAll() const;

 private:
  std::vector<std::string> all_;
  std::vector<CLIOption> options_;
  std::vector<std::string> positionals_;
};

/** A one shot request an app takes from the command line, such as --repair. */
struct CLIAction {
  std::string_view name; /**< Name without the leading dashes. */
  std::string_view description;
};

struct CLIActionHit {
  CLIAction action;
  CLIOption option;
};

namespace detail {

/**
 * Reports whether a character can appear in an action name.
 *
 * @return True when c is a lowercase letter, a digit, an underscore or a hyphen.
 */
constexpr bool IsActionChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/**
 * Reports whether a name can be an action. It starts with a lowercase letter and holds lowercase
 * letters, digits, underscores and hyphens.
 */
constexpr bool IsActionName(std::string_view name) {
  return !name.empty() && name.front() >= 'a' && name.front() <= 'z' &&
         std::ranges::all_of(name, IsActionChar);
}

/**
 * Builds an action and stops the compile on an invalid name.
 *
 * @param description One line that says what the action does.
 */
consteval CLIAction MakeAction(std::string_view name, std::string_view description) {
  if (!IsActionName(name)) {
    throw "an action name is lowercase letters, digits, underscores and hyphens";
  }
  return CLIAction{name, description};
}

}  // namespace detail

}  // namespace rex

/**
 * Declares a command line action. The name is a bare token, so no dotted name can be declared.
 *
 * @param name The action name without quotes, such as repair.
 * @param description One line that says what the action does.
 * @code
 * static constexpr std::array<rex::CLIAction, 1> kActions = {
 *     REX_CLI_ACTION(repair, "Opens the installer wizard in repair mode.")};
 * @endcode
 */
#define REX_CLI_ACTION(name, description) ::rex::detail::MakeAction(#name, description)
