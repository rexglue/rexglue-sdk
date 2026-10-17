/**
 * @file        tests/unit/rexglue/project_scan_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "support/temp_folder.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

#include "rexglue/commands/project_scan.h"

REX_TEST_CASE("stale include scan matches header names without regard to case",
              "[rexglue][scan][case]") {
  rex::test::TempFolder root("rexglue_scan_");
  std::filesystem::create_directories(root.path / "src");
  std::ofstream(root.path / "src" / "main.CPP")
      << "#include \"out/DEMO_funcs.h\"\n#include \"keep.h\"\n#include <Demo_Init.H>\n";
  std::ofstream(root.path / "src" / "notes.TXT") << "#include \"demo_funcs.h\"\n";
  const auto warnings = rexglue::cli::ScanStaleIncludes(
      root.path / "src", std::unordered_set<std::string>{"demo_funcs.h", "demo_init.h"});
  REQUIRE(warnings.size() == 2);
  CHECK(warnings[0].line_number == 1);
  CHECK(warnings[1].line_number == 3);
}
