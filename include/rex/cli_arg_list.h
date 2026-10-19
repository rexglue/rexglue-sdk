/**
 * @file        cli_arg_list.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rex/platform.h>
#include <rex/export.h>
#include <rex/types.h>

namespace rex {

/** An option of a command line. */
struct CLIOption {
  /** Name without the leading dashes. */
  std::string name;
  /** Value of --name=value or --name value, empty when the option has none. */
  std::optional<std::string> value;
};

/**
 * The command line a process was started with, split into options and positional arguments.
 * An option is --name or --name=value. A token that follows --name and does not start with --
 * is that option's value.
 */
class REX_API CLIArgList {
 public:
  /**
   * Parses the command line main receives.
   *
   * @param argc Argument count, including the program name.
   * @param argv Argument vector. argv[0] is the program name and is skipped.
   * @return The parsed arguments.
   */
  static CLIArgList Parse(i32 argc, char** argv);

  /**
   * Parses command line arguments held as strings.
   *
   * @param args Command line arguments without the program name.
   * @return The parsed arguments.
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

  /**
   * Returns the options in the order given.
   *
   * @return Each option with its name and its value, if it has one.
   */
  std::span<const CLIOption> GetOptions() const;

  /**
   * Returns the arguments that are not options or option values.
   *
   * @return The positional arguments, in order.
   */
  std::span<const std::string> GetPositionals() const;

  /**
   * Returns every argument.
   *
   * @return The arguments in their original order, without the program name.
   */
  std::span<const std::string> GetAll() const;

 private:
  std::vector<std::string> all_;
  std::vector<CLIOption> options_;
  std::vector<std::string> positionals_;
};

}  // namespace rex
