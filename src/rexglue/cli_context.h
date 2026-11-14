/**
 * @file        rexglue/cli_context.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/result.h>
#include <rex/utility.h>

namespace rex::codegen {
class ToolApp;
}  // namespace rex::codegen

namespace rexglue::cli {

struct CLIContext {
  bool verbose = false;
  bool overwrite_existing = false;
  bool generate_despite_errors = false;
  rex::codegen::ToolApp* tools = nullptr;
};

using DeferredAction = rex::MoveOnlyFunction<rex::Result<void>()>;

}  // namespace rexglue::cli
