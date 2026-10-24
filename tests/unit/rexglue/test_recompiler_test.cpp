/**
 * @file        tests/unit/rexglue/test_recompiler_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "file_text.h"
#include "temp_folder.h"

#include <filesystem>
#include <string>

#include "rexglue/commands/test_recompiler.h"
#include "rexglue/ui/ui.h"

namespace fs = std::filesystem;

namespace {

using rex::test::WriteText;

struct UIScope {
  UIScope() { rexglue::ui::Init({.tty = false, .color = false}); }
  ~UIScope() { rexglue::ui::Shutdown(); }
};

void WriteBlr(const fs::path& path) {
  WriteText(path, std::string("\x4E\x80\x00\x20", 4));
}

}  // namespace

REX_TEST_CASE("Recompiling tests fails when the binary folder is missing",
              "[rexglue][recompile_tests]") {
  UIScope ui;
  rex::test::TempFolder folder("rexglue_recompile_missing_");
  CHECK_FALSE(rexglue::cli::RecompileTests((folder.path / "none").string(), folder.path.string(),
                                           (folder.path / "out").string()));
}

REX_TEST_CASE("Recompiling tests fails when the binary folder holds no binaries",
              "[rexglue][recompile_tests]") {
  UIScope ui;
  rex::test::TempFolder folder("rexglue_recompile_empty_");
  CHECK_FALSE(rexglue::cli::RecompileTests(folder.path.string(), folder.path.string(),
                                           (folder.path / "out").string()));
}

REX_TEST_CASE("Recompiling tests fails when a binary has no symbols",
              "[rexglue][recompile_tests]") {
  UIScope ui;
  rex::test::TempFolder folder("rexglue_recompile_nomap_");
  WriteBlr(folder.path / "instr_add.bin");
  CHECK_FALSE(rexglue::cli::RecompileTests(folder.path.string(), folder.path.string(),
                                           (folder.path / "out").string()));
}

REX_TEST_CASE("Recompiling tests writes the generated files", "[rexglue][recompile_tests]") {
  UIScope ui;
  rex::test::TempFolder folder("rexglue_recompile_ok_");
  WriteBlr(folder.path / "instr_add.bin");
  WriteText(folder.path / "instr_add.map", "00000000 T test_add\n");
  WriteText(folder.path / "instr_add.s",
            "test_add:\n  #_ REGISTER_IN r3 1\n  blr\n  #_ REGISTER_OUT r3 1\n");
  const fs::path out = folder.path / "out";
  REQUIRE(rexglue::cli::RecompileTests(folder.path.string(), folder.path.string(), out.string()));
  CHECK(fs::exists(out / "ppc_config.h"));
  CHECK(fs::exists(out / "ppc_test_cases.cpp"));
}
