/**
 * @file        cli_action.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <string_view>

#include <rex/cli_arg_list.h>

namespace rex {

/** A one shot request an app takes from the command line, such as --repair. */
struct CLIAction {
  std::string_view name;        /**< Name without the leading dashes. */
  std::string_view description; /**< 1 line that says what the action does. */
};

/** An action found on the command line with the option that named it. */
struct CLIActionHit {
  CLIAction action; /**< The declared action. */
  CLIOption option; /**< The parsed option, with its value if it has one. */
};

namespace detail {

/**
 * Reports whether a character can appear in an action name.
 *
 * @param c The character.
 * @return True when c is a lowercase letter, a digit, an underscore or a hyphen.
 */
constexpr bool IsActionChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/**
 * Reports whether a name can be an action. It starts with a lowercase letter and holds lowercase
 * letters, digits, underscores and hyphens, which keeps it apart from every dotted cvar name.
 *
 * @param name The candidate name.
 * @return True when the name is valid.
 */
constexpr bool IsActionName(std::string_view name) {
  return !name.empty() && name.front() >= 'a' && name.front() <= 'z' &&
         std::ranges::all_of(name, IsActionChar);
}

/**
 * Builds an action and stops the compile on an invalid name.
 *
 * @param name The action name.
 * @param description 1 line that says what the action does.
 * @return The action.
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
 * An app returns its actions from App::GetActions and handles each in App::OnAction.
 *
 * @param name The action name without quotes, such as repair.
 * @param description 1 line that says what the action does.
 * @code
 * static constexpr std::array<rex::CLIAction, 1> kActions = {
 *     REX_CLI_ACTION(repair, "Opens the installer wizard in repair mode.")};
 * @endcode
 */
#define REX_CLI_ACTION(name, description) ::rex::detail::MakeAction(#name, description)
