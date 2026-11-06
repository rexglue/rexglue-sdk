/**
 * @file        rexglue/command/test_recompiler.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string_view>

#include "../cli_context.h"

namespace CLI {
class App;
}

namespace rexglue::cli {

/**
 * Generates Catch2 test sources from linked PPC test binaries.
 *
 * @param bin_dir The folder of linked `.bin` files with a `.map` file of the same name.
 * @param asm_dir The folder of `.s` sources that hold the test directives.
 * @param out_dir The folder to write the generated files into.
 * @param case_files The number of `ppc_test_cases_N.cpp` files to split the test cases into, at
 *        least 1.
 * @return True when every binary was processed and every file was written.
 */
bool RecompileTests(std::string_view bin_dir, std::string_view asm_dir, std::string_view out_dir,
                    size_t case_files = 1);

void RegisterRecompileTests(CLI::App& parent, const CLIContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
