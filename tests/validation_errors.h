/**
 * @file        tests/validation_errors.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "codegen/config.h"

namespace rex::test {

inline bool HasError(const codegen::RecompilerConfig::ValidationResult& result,
                     std::string_view text) {
  return std::ranges::any_of(result.errors, [text](const std::string& error) {
    return error.find(text) != std::string::npos;
  });
}

}  // namespace rex::test
