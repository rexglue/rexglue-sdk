/**
 * @file        tests/unit/codegen/hook_scan_test.cpp
 * @brief       Codegen reads mid asm hooks out of C++ sources
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/hook_scan.h>

namespace fs = std::filesystem;

namespace {

using rex::codegen::MidAsmKind;
using rex::codegen::SourceMidAsmHook;

void Write(const fs::path& path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

rex::Result<std::vector<SourceMidAsmHook>> Scan(const rex::test::TempFolder& folder,
                                                std::vector<fs::path>* scanned = nullptr) {
  std::vector<fs::path> files;
  const fs::path roots[] = {folder.path / "src"};
  auto result = rex::codegen::ScanHookSources(roots, files);
  if (scanned) {
    *scanned = files;
  }
  return result;
}

std::string ErrorOf(const rex::Result<std::vector<SourceMidAsmHook>>& result) {
  return result ? std::string() : result.error().message;
}

std::vector<SourceMidAsmHook> Hooks(const rex::Result<std::vector<SourceMidAsmHook>>& result) {
  INFO(ErrorOf(result));
  REQUIRE(result);
  return *result;
}

}  // namespace

REX_TEST_CASE("hook_scan: each body kind is read with its attachments", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        R"(REX_MIDASM_HOOK(ScaleStep, (PPCRegister& step)) {
  step.f64 *= 2.0;
}
REX_MIDASM_AT(ScaleStep, 0x82000100, after, f10);

REX_MIDASM_BRANCH(SkipRead, (u32 size, u32 op)) {
  return size == 0;
}
REX_MIDASM_AT(SkipRead, 0x82000300, before, 0x8200030C, r5, r31);

REX_MIDASM_RETURN_IF(SkipAll, (u32 count)) {
  return count == 0;
}
REX_MIDASM_AT(SkipAll, 0x82000400, before, r3);
)");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 3);
  CHECK(hooks[0].name == "ScaleStep");
  CHECK(hooks[0].address == 0x82000100);
  CHECK(hooks[0].after);
  CHECK(hooks[0].kind == MidAsmKind::kHook);
  CHECK(hooks[0].target == 0);
  CHECK(hooks[0].registers == std::vector<std::string>{"f10"});
  CHECK(hooks[0].parameters == std::vector<std::string>{"PPCRegister&"});
  CHECK(hooks[0].line == 4);
  CHECK(hooks[1].name == "SkipRead");
  CHECK_FALSE(hooks[1].after);
  CHECK(hooks[1].kind == MidAsmKind::kBranch);
  CHECK(hooks[1].target == 0x8200030C);
  CHECK(hooks[1].registers == std::vector<std::string>{"r5", "r31"});
  CHECK(hooks[1].parameters == std::vector<std::string>{"u32", "u32"});
  CHECK(hooks[2].kind == MidAsmKind::kReturnIf);
  CHECK(hooks[2].registers == std::vector<std::string>{"r3"});
}

REX_TEST_CASE("hook_scan: a body with no parameters is attached with no registers",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        R"(REX_MIDASM_RETURN_IF(NothingToRead, ()) {
  return true;
}
REX_MIDASM_AT(NothingToRead, 0x82000500, before);
)");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 1);
  CHECK(hooks[0].kind == MidAsmKind::kReturnIf);
  CHECK(hooks[0].registers.empty());
  CHECK(hooks[0].parameters.empty());
}

REX_TEST_CASE("hook_scan: one body attached at 3 addresses in another file",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "body.cpp", "REX_MIDASM_HOOK(Shared, (u64 value)) {}\n");
  Write(folder.path / "src" / "deep" / "attach.h",
        "REX_MIDASM_AT(Shared, 0x82000200, after, r3);\n"
        "REX_MIDASM_AT(Shared, 0x82000100, before, r4);\n"
        "REX_MIDASM_AT(Shared, 0x82000300, before, r5);\n");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 3);
  CHECK(hooks[0].address == 0x82000100);
  CHECK(hooks[1].address == 0x82000200);
  CHECK(hooks[2].address == 0x82000300);
  CHECK(hooks[0].registers == std::vector<std::string>{"r4"});
  CHECK(hooks[0].parameters == std::vector<std::string>{"u64"});
}

REX_TEST_CASE("hook_scan: every register kind and parameter kind is read", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        R"(REX_MIDASM_HOOK(EveryKind,
                (PPCContext & ctx, u32 a, u64 b, f64 c, PPCRegister& d, PPCVRegister e,
                 PPCVRegister& f, PPCCRRegister& g, PPCRegister& h, PPCXERRegister& i,
                 PPCFPSCRRegister& j, PPCRegister& k)) {}
REX_MIDASM_AT(EveryKind, 0x82000480, after, ctx, r3, r4, f1, r5, v1, v2, cr6, ctr, xer,
              fpscr, reserved);
)");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 1);
  CHECK(hooks[0].registers == std::vector<std::string>{"ctx", "r3", "r4", "f1", "r5", "v1", "v2",
                                                       "cr6", "ctr", "xer", "fpscr", "reserved"});
  CHECK(hooks[0].parameters ==
        std::vector<std::string>{"PPCContext&", "u32", "u64", "f64", "PPCRegister&", "PPCVRegister",
                                 "PPCVRegister&", "PPCCRRegister&", "PPCRegister&",
                                 "PPCXERRegister&", "PPCFPSCRRegister&", "PPCRegister&"});
}

REX_TEST_CASE("hook_scan: pointer and mapped pointer parameters are read as erased types",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        R"(REX_MIDASM_HOOK(Pointers,
                (Widget* widget, const Widget* other, rex::MappedPtr<Widget> mapped,
                 MappedPtr<Widget> bare, u8* raw, Widget**, MappedPtr<u32>)) {}
REX_MIDASM_AT(Pointers, 0x82000100, before, r27, r28, r29, r30, r31, r26, r25);
)");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 1);
  CHECK(hooks[0].registers ==
        std::vector<std::string>{"r27", "r28", "r29", "r30", "r31", "r26", "r25"});
  CHECK(hooks[0].parameters == std::vector<std::string>{"void*", "void*", "rex::MappedPtr<u8>",
                                                        "rex::MappedPtr<u8>", "void*", "void*",
                                                        "rex::MappedPtr<u8>"});
}

REX_TEST_CASE("hook_scan: the spellings the macros accept are read", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        R"(REX_MIDASM_HOOK(Spelled,
                (rex::u32 a, const u32 b, ::rex::u64 c, double d, MappedU32 m, rex::MappedU8 n,
                 uint32_t e, GuestPtrT<u16> g)) {}
REX_MIDASM_AT(Spelled, 0x8200'0100u, after, r3, r4, r5, f1, r6, r7, r8, r9);
REX_MIDASM_BRANCH(Jump, (u32 a)) { return a == 0; }
REX_MIDASM_AT(Jump, 2181038592, before, 0x8200'0210ul, r3);
)");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 2);
  CHECK(hooks[0].address == 0x82000100);
  CHECK(hooks[0].parameters == std::vector<std::string>{"u32", "u32", "u64", "f64",
                                                        "rex::MappedPtr<u8>", "rex::MappedPtr<u8>",
                                                        "u32", "rex::MappedPtr<u8>"});
  CHECK(hooks[1].address == 0x82000200);
  CHECK(hooks[1].target == 0x82000210);
}

REX_TEST_CASE("hook_scan: a const reference parameter is refused", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_HOOK(Constant, (const PPCRegister& value)) {}\n");
  CHECK_FALSE(Scan(folder));
}

REX_TEST_CASE("hook_scan: a pointer parameter needs an integer register", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_HOOK(Pointer, (Widget* widget)) {}\n"
        "REX_MIDASM_AT(Pointer, 0x82000100, before, f1);\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("does not fit"));
}

REX_TEST_CASE("hook_scan: a function pointer parameter stops", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp", "REX_MIDASM_HOOK(Callback, (void (*callback)())) {}\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:1:"));
}

REX_TEST_CASE("hook_scan: a macro wrapped over 4 lines is read", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_BRANCH(Wrapped, (u32 a,\n"
        "                            u32 b)) {\n"
        "  return a == b;\n"
        "}\n"
        "REX_MIDASM_AT(Wrapped,\n"
        "              0x82000100,\n"
        "              before,\n"
        "              0x82000110,\n"
        "              r3,\n"
        "              r4);\n");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 1);
  CHECK(hooks[0].line == 5);
  CHECK(hooks[0].target == 0x82000110);
  CHECK(hooks[0].registers == std::vector<std::string>{"r3", "r4"});
}

REX_TEST_CASE("hook_scan: macros in comments, strings and definitions are ignored",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "// REX_MIDASM_AT(Ghost, 0x82000100, after, r3);\n"
        "/* REX_MIDASM_AT(Ghost, 0x82000104, after, r3);\n"
        "   REX_MIDASM_AT(Ghost, 0x82000108, after, r3); */\n"
        "const char* kText = \"REX_MIDASM_AT(Ghost, 0x8200010C, after, r3);\";\n"
        "const char* kRaw = R\"x(REX_MIDASM_AT(Ghost, 0x82000110, after, r3);)x\";\n"
        "#define LOCAL_ATTACH REX_MIDASM_AT(Ghost, 0x82000114, after, r3);\n"
        "#define LOCAL_LONG(x) \\\n"
        "  REX_MIDASM_AT(Ghost, 0x82000118, after, r3)\n"
        "REX_MIDASM_HOOK(Real, (u32 value)) {}\n"
        "REX_MIDASM_AT(Real, 0x82000200, after, r3);\n");
  const auto hooks = Hooks(Scan(folder));
  REQUIRE(hooks.size() == 1);
  CHECK(hooks[0].name == "Real");
  CHECK(hooks[0].line == 10);
}

REX_TEST_CASE("hook_scan: only the source files under the roots are read", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "a.cpp", "int a;\n");
  Write(folder.path / "src" / "b.cc", "int b;\n");
  Write(folder.path / "src" / "c.h", "int c;\n");
  Write(folder.path / "src" / "d.hpp", "int d;\n");
  Write(folder.path / "src" / "notes.txt", "REX_MIDASM_AT(Ghost, 0x82000100, after, r3);\n");
  Write(folder.path / "other" / "e.cpp", "int e;\n");
  std::vector<fs::path> scanned;
  const auto hooks = Hooks(Scan(folder, &scanned));
  CHECK(hooks.empty());
  const auto sources =
      std::ranges::count_if(scanned, [](const fs::path& path) { return path.has_extension(); });
  CHECK(sources == 4);
  CHECK(std::ranges::none_of(scanned, [](const fs::path& path) {
    return path.extension() == ".txt" || path.filename() == "e.cpp" || path.filename() == "other";
  }));
}

REX_TEST_CASE("hook_scan: every folder searched is reported beside the files",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "a.cpp", "int a;\n");
  Write(folder.path / "src" / "deep" / "inner" / "b.cpp", "int b;\n");
  std::vector<fs::path> scanned;
  Hooks(Scan(folder, &scanned));
  const auto has = [&](const fs::path& path) {
    return std::ranges::find(scanned, path) != scanned.end();
  };
  CHECK(has(folder.path / "src"));
  CHECK(has(folder.path / "src" / "deep"));
  CHECK(has(folder.path / "src" / "deep" / "inner"));
  CHECK(has(folder.path / "src" / "a.cpp"));
  CHECK(has(folder.path / "src" / "deep" / "inner" / "b.cpp"));
}

REX_TEST_CASE("hook_scan: a header with an include guard is not a conditional",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "guarded.h",
        "#ifndef GUARDED_H\n#define GUARDED_H\n"
        "REX_MIDASM_HOOK(Guarded, (u32 value)) {}\n"
        "REX_MIDASM_AT(Guarded, 0x82000100, after, r3);\n"
        "#endif\n");
  const auto hooks = Hooks(Scan(folder));
  CHECK(hooks.size() == 1);
}

REX_TEST_CASE("hook_scan: a macro inside a conditional stops with its file and line",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_HOOK(Gated, (u32 value)) {}\n"
        "#if FEATURE\n"
        "REX_MIDASM_AT(Gated, 0x82000100, after, r3);\n"
        "#endif\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:3:"));
  CHECK(ErrorOf(result).contains("preprocessor conditional"));
}

REX_TEST_CASE("hook_scan: an unreadable macro stops with its file and line",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "int before_it;\n"
        "REX_MIDASM_HOOK(Broken, (u32 value) {}\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:2:"));
}

REX_TEST_CASE("hook_scan: an attachment for an unknown body stops with its line",
              "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp", "\n\nREX_MIDASM_AT(Missing, 0x82000100, after, r3);\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:3:"));
  CHECK(ErrorOf(result).contains("Missing"));
}

REX_TEST_CASE("hook_scan: 2 hooks at 1 address stop with both locations", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_HOOK(First, (u32 value)) {}\n"
        "REX_MIDASM_HOOK(Second, (u32 value)) {}\n"
        "REX_MIDASM_AT(First, 0x82000100, after, r3);\n"
        "REX_MIDASM_AT(Second, 0x82000100, before, r3);\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:4:"));
  CHECK(ErrorOf(result).contains("hooks.cpp:3"));
  CHECK(ErrorOf(result).contains("0x82000100"));
}

REX_TEST_CASE("hook_scan: a body declared twice stops", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp",
        "REX_MIDASM_HOOK(Twice, (u32 value)) {}\n"
        "REX_MIDASM_HOOK(Twice, (u32 value)) {}\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:2:"));
}

REX_TEST_CASE("hook_scan: a parameter type no register fills stops", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  Write(folder.path / "src" / "hooks.cpp", "REX_MIDASM_HOOK(Narrow, (f32 value)) {}\n");
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hooks.cpp:1:"));
  CHECK(ErrorOf(result).contains("f32"));
}

REX_TEST_CASE("hook_scan: the scanner rejects the registers the macro parser rejects",
              "[codegen][hook_scan]") {
  const char* const tokens[] = {"r1", "v128", "r32", "f32", "cr8", "lr", "r01"};
  for (const char* token : tokens) {
    rex::test::TempFolder folder;
    Write(folder.path / "src" / "hooks.cpp",
          std::string("REX_MIDASM_HOOK(Body, (PPCRegister& value)) {}\n"
                      "REX_MIDASM_AT(Body, 0x82000100, after, ") +
              token + ");\n");
    const auto result = Scan(folder);
    INFO(token);
    REQUIRE_FALSE(result);
    CHECK(ErrorOf(result).contains("hooks.cpp:2:"));
    CHECK(ErrorOf(result).contains("register"));
  }
}

REX_TEST_CASE("hook_scan: the scanner accepts the registers the macro parser accepts",
              "[codegen][hook_scan]") {
  struct Case {
    const char* token;
    const char* type;
  };
  const Case accepted[] = {
      {"r0", "u32"},
      {"r31", "u64"},
      {"f0", "f64"},
      {"f31", "PPCRegister&"},
      {"v0", "PPCVRegister"},
      {"v127", "PPCVRegister&"},
      {"cr0", "PPCCRRegister&"},
      {"cr7", "PPCCRRegister&"},
      {"ctr", "PPCRegister&"},
      {"xer", "PPCXERRegister&"},
      {"fpscr", "PPCFPSCRRegister&"},
      {"reserved", "PPCRegister&"},
      {"ctx", "PPCContext&"},
  };
  for (const Case& test : accepted) {
    rex::test::TempFolder folder;
    Write(folder.path / "src" / "hooks.cpp",
          std::string("REX_MIDASM_HOOK(Body, (") + test.type +
              " value)) {}\nREX_MIDASM_AT(Body, 0x82000100, after, " + test.token + ");\n");
    const auto result = Scan(folder);
    INFO(test.token);
    INFO(ErrorOf(result));
    CHECK(result);
  }
}

REX_TEST_CASE("hook_scan: register count, kind and address problems stop", "[codegen][hook_scan]") {
  struct Case {
    const char* attach;
    const char* expect;
  };
  const Case cases[] = {
      {"REX_MIDASM_AT(Body, 0x82000100, after, r3, r4);", "register count"},
      {"REX_MIDASM_AT(Body, 0x82000100, after, f1);", "does not fit"},
      {"REX_MIDASM_AT(Body, 0x82000102, after, r3);", "multiple of 4"},
      {"REX_MIDASM_AT(Body, kAddress, after, r3);", "literal"},
      {"REX_MIDASM_AT(Body, 0x82000100, during, r3);", "before or after"},
      {"REX_MIDASM_AT(Body, 0x82000100, after, 0x82000110, r3);", "only a branch body"},
  };
  for (const Case& test : cases) {
    rex::test::TempFolder folder;
    Write(folder.path / "src" / "hooks.cpp",
          std::string("REX_MIDASM_HOOK(Body, (u32 value)) {}\n") + test.attach + "\n");
    const auto result = Scan(folder);
    INFO(test.attach);
    REQUIRE_FALSE(result);
    CHECK(ErrorOf(result).contains("hooks.cpp:2:"));
    CHECK(ErrorOf(result).contains(test.expect));
  }
}

REX_TEST_CASE("hook_scan: a missing folder stops", "[codegen][hook_scan]") {
  rex::test::TempFolder folder;
  const auto result = Scan(folder);
  REQUIRE_FALSE(result);
  CHECK(ErrorOf(result).contains("hook_sources"));
}

REX_TEST_CASE("hook_scan: a source hook converts to the hook entry", "[codegen][hook_scan]") {
  SourceMidAsmHook branch{"Name", 0x82000100, true,        0x82000110, MidAsmKind::kBranch,
                          {"r3"}, {"u32"},    "hooks.cpp", 1};
  const auto entry = rex::codegen::ToMidAsmHook(branch);
  CHECK(entry.from_source);
  CHECK(entry.after_instruction);
  CHECK(entry.jump_address_on_true == 0x82000110);
  CHECK_FALSE(entry.return_on_true);
  CHECK(entry.registers == std::vector<std::string>{"r3"});
  CHECK(entry.parameters == std::vector<std::string>{"u32"});

  SourceMidAsmHook skip{"Skip", 0x82000100, false,       0, MidAsmKind::kReturnIf,
                        {"r3"}, {"u32"},    "hooks.cpp", 1};
  const auto skip_entry = rex::codegen::ToMidAsmHook(skip);
  CHECK(skip_entry.return_on_true);
  CHECK(skip_entry.jump_address_on_true == 0);
}
