/**
 * @file        tests/unit/codegen/midasm_hook_emit_test.cpp
 * @brief       The mid function hook wrapper skips the red zone and restores r1 on every exit
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/codegen/output_stamp.h>
#include <rex/platform/env.h>

namespace {

using rex::i32;
using rex::u32;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kLiR3 = 0x38600001;
constexpr u32 kLiR4 = 0x38800002;
constexpr u32 kLiR5 = 0x38A00003;
constexpr u32 kHookIndex = 1;
constexpr u32 kBranchIndex = 2;
constexpr u32 kTargetIndex = 4;

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

constexpr u32 Branch(u32 from_index, u32 to_index) {
  return 0x48000000 |
         ((static_cast<i32>(to_index) - static_cast<i32>(from_index)) * 4 & 0x03FFFFFC);
}

std::string GenerateWithHook(std::string_view hook_fields) {
  const std::vector<u32> words = {kLiR3, kLiR4, Branch(kBranchIndex, kTargetIndex), kLiR5, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), 5}};
  rex::test::TempFolder folder;
  const std::string manifest = std::format("[[entrypoint.midasm_hook]]\naddress = 0x{:X}\n{}\n",
                                           Address(kHookIndex), hook_fields);
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

size_t Count(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

constexpr std::string_view kSkip = "ctx.r1.u64 -= rex::ppc::kRedZone;";
constexpr std::string_view kRestore = "ctx.r1.u64 += rex::ppc::kRedZone;";

}  // namespace

REX_TEST_CASE("A mid function hook skips the red zone before the call and restores r1 after it",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook("name = \"PlainHook\"");
  const auto skip = body.find(kSkip);
  const auto call = body.find("PlainHook(");
  const auto restore = body.find(kRestore);
  REQUIRE(skip != std::string::npos);
  REQUIRE(call != std::string::npos);
  REQUIRE(restore != std::string::npos);
  CHECK(skip < call);
  CHECK(call < restore);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook that returns restores r1 before the return",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook("name = \"ReturnHook\"\nreturn = true");
  const auto restore = body.find(kRestore);
  const auto early_return = body.find("return;", restore);
  REQUIRE(restore != std::string::npos);
  REQUIRE(early_return != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook that jumps restores r1 before the jump",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook(
      std::format("name = \"JumpHook\"\njump_address = 0x{:X}", Address(kTargetIndex)));
  const auto restore = body.find(kRestore);
  const auto jump = body.find(std::format("goto loc_{:X};", Address(kTargetIndex)), restore);
  REQUIRE(restore != std::string::npos);
  REQUIRE(jump != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 1);
}

REX_TEST_CASE("A mid function hook with a condition restores r1 on both exits",
              "[codegen][midasm][redzone]") {
  const auto body = GenerateWithHook(
      std::format("name = \"BranchHook\"\nreturn_on_true = true\njump_address_on_false = 0x{:X}",
                  Address(kTargetIndex)));
  REQUIRE(body.find("if (BranchHook(") != std::string::npos);
  CHECK(Count(body, kSkip) == 1);
  CHECK(Count(body, kRestore) == 2);
  const auto taken = body.find(kRestore);
  const auto not_taken = body.find(kRestore, taken + 1);
  REQUIRE(not_taken != std::string::npos);
  CHECK(body.find("return;", taken) < not_taken);
  CHECK(body.find(std::format("goto loc_{:X};", Address(kTargetIndex)), not_taken) !=
        std::string::npos);
}

namespace {

constexpr std::string_view kSourceKeys = "hook_sources = [\"src\"]\n";

rex::Result<void> RunWithSource(const rex::test::TempFolder& folder, std::string_view source,
                                std::string_view extra_manifest = {}) {
  std::filesystem::create_directories(folder.path / "src");
  std::ofstream(folder.path / "src" / "hooks.cpp") << source;
  const std::vector<u32> words = {kLiR3, kLiR4, Branch(kBranchIndex, kTargetIndex), kLiR5, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), 5}};
  return rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata),
                                      extra_manifest, true, {}, nullptr, kSourceKeys);
}

std::string GenerateSourceTree(const rex::test::TempFolder& folder, std::string_view source) {
  const auto result = RunWithSource(folder, source);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

std::string FunctionBody(const std::string& tree) {
  const auto begin = tree.find("DEFINE_REX_FUNC(xstart)");
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find("\n}\n", begin) - begin);
}

std::string ArgumentsOf(const std::string& text, std::string_view call) {
  const auto begin = text.find(call);
  REQUIRE(begin != std::string::npos);
  const auto open = begin + call.size();
  return text.substr(open, text.find(')', open) - open);
}

std::string Attach(std::string_view name, std::string_view placement, std::string_view tail) {
  return std::format("REX_MIDASM_AT({}, 0x{:X}, {}, {});\n", name, Address(kHookIndex), placement,
                     tail);
}

std::string DeclarationOf(const std::string& tree, std::string_view name) {
  const std::string prefix = std::format("extern \"C\" void {}(", name);
  const auto begin = tree.find(prefix);
  REQUIRE(begin != std::string::npos);
  return tree.substr(begin, tree.find(';', begin) + 1 - begin) + "\n";
}

std::string CallOf(const std::string& tree, std::string_view name) {
  const std::string body = FunctionBody(tree);
  const auto begin = body.find(std::format("\t{}(", name));
  REQUIRE(begin != std::string::npos);
  return body.substr(begin, body.find(");", begin) + 2 - begin) + "\n";
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

REX_TEST_CASE("A source hook with no parameters declares and calls an empty list",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto tree = GenerateSourceTree(
      folder, "REX_MIDASM_HOOK(SrcNone, ()) {}\n" +
                  std::format("REX_MIDASM_AT(SrcNone, 0x{:X}, before);\n", Address(kHookIndex)));
  CHECK(tree.contains("extern \"C\" void SrcNone();"));
  CHECK(FunctionBody(tree).contains("SrcNone();"));
}

REX_TEST_CASE("A source hook declares an extern C prototype and passes values and references",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto tree =
      GenerateSourceTree(folder, "REX_MIDASM_HOOK(SrcPlain, (u32 size, PPCRegister& held)) {}\n" +
                                     Attach("SrcPlain", "before", "r3, r4"));
  CHECK(tree.contains("extern \"C\" void SrcPlain(u32, PPCRegister&);"));
  CHECK(FunctionBody(tree).contains("SrcPlain(ctx.r3.u32, ctx.r4);"));
}

REX_TEST_CASE("A source hook passes u64 and f64 values and a vector by value",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto tree = GenerateSourceTree(
      folder, "REX_MIDASM_HOOK(SrcWide, (u64 wide, f64 real, PPCVRegister vec)) {}\n" +
                  Attach("SrcWide", "after", "r5, f2, v6"));
  CHECK(tree.contains("extern \"C\" void SrcWide(u64, f64, PPCVRegister);"));
  CHECK(FunctionBody(tree).contains("SrcWide(ctx.r5.u64, ctx.f2.f64, ctx.v6);"));
}

REX_TEST_CASE("A source hook passes pointers and mapped pointers through the argument translator",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto tree = GenerateSourceTree(
      folder, "REX_MIDASM_HOOK(SrcPointers, (Widget* widget, rex::MappedPtr<Widget> mapped)) {}\n" +
                  Attach("SrcPointers", "before", "r27, r28"));
  CHECK(tree.contains("extern \"C\" void SrcPointers(void*, rex::MappedPtr<u8>);"));
  CHECK(FunctionBody(tree).contains(
      "SrcPointers(rex::ppc::ArgTranslator::GetPointerValue<void*>(ctx.r27.u64, base), "
      "rex::ppc::ArgTranslator::GetMappedPointerValue<rex::MappedPtr<u8>>(ctx.r28.u64, base));"));
}

REX_TEST_CASE(
    "The generated declarations and calls of source hooks are written for the compile test",
    "[codegen][midasm][hook_source]") {
  struct Hook {
    const char* name;
    const char* source;
    const char* registers;
  };
  const Hook hooks[] = {
      {"GeneratedPlain", "REX_MIDASM_HOOK(GeneratedPlain, (u32 size, PPCRegister& held)) {}\n",
       "r3, r4"},
      {"GeneratedPointers",
       "REX_MIDASM_HOOK(GeneratedPointers, (GeneratedWidget* widget, "
       "rex::MappedPtr<GeneratedWidget> mapped)) {}\n",
       "r27, r28"},
      {"GeneratedConstPointer",
       "REX_MIDASM_HOOK(GeneratedConstPointer, (const GeneratedWidget* widget)) {}\n", "r5"},
  };
  std::string declarations;
  std::string calls;
  for (const Hook& hook : hooks) {
    rex::test::TempFolder folder;
    const auto tree =
        GenerateSourceTree(folder, hook.source + Attach(hook.name, "before", hook.registers));
    declarations += DeclarationOf(tree, hook.name);
    calls += CallOf(tree, hook.name);
  }
  CHECK(declarations.contains("GeneratedPointers(void*, rex::MappedPtr<u8>);"));
  CHECK(declarations.contains("GeneratedConstPointer(void*);"));
  if (const auto folder = rex::platform::GetEnvironmentValue("REX_MIDASM_GENERATED_DIR")) {
    WriteFile(std::filesystem::path(*folder) / "declarations.inc", declarations);
    WriteFile(std::filesystem::path(*folder) / "calls.inc", calls);
  }
}

REX_TEST_CASE("A source hook passes the context for a ctx parameter",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto tree = GenerateSourceTree(
      folder, "REX_MIDASM_HOOK(SrcContext, (PPCContext & state, u32 value)) {}\n" +
                  Attach("SrcContext", "before", "ctx, r3"));
  CHECK(tree.contains("extern \"C\" void SrcContext(PPCContext&, u32);"));
  CHECK(FunctionBody(tree).contains("SrcContext(ctx, ctx.r3.u32);"));
}

REX_TEST_CASE("A source hook passes cr, ctr, xer, fpscr and reserved as a TOML hook does",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder source_folder;
  const auto source_tree = GenerateSourceTree(
      source_folder,
      "REX_MIDASM_HOOK(SrcState, (PPCCRRegister & cr, PPCRegister& counter, PPCXERRegister& xer, "
      "PPCFPSCRRegister& fpscr, PPCRegister& held)) {}\n" +
          Attach("SrcState", "before", "cr6, ctr, xer, fpscr, reserved"));
  const auto toml_body = GenerateWithHook(
      "name = \"TomlState\"\nregisters = [\"cr6\", \"ctr\", \"xer\", \"fpscr\", \"reserved\"]");
  const auto source_arguments = ArgumentsOf(FunctionBody(source_tree), "SrcState(");
  CHECK(source_arguments == ArgumentsOf(toml_body, "TomlState("));
  CHECK_FALSE(source_arguments.empty());
}

REX_TEST_CASE("A source branch hook jumps to its target and a return hook returns",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder branch_folder;
  const auto branch = FunctionBody(GenerateSourceTree(
      branch_folder,
      "REX_MIDASM_BRANCH(SrcBranch, (u32 value)) { return true; }\n" +
          Attach("SrcBranch", "before", std::format("0x{:X}, r3", Address(kTargetIndex)))));
  CHECK(branch.contains("if (SrcBranch(ctx.r3.u32)) {"));
  CHECK(branch.contains(std::format("goto loc_{:X};", Address(kTargetIndex))));
  CHECK(Count(branch, kRestore) == 2);

  rex::test::TempFolder return_folder;
  const auto tree = GenerateSourceTree(
      return_folder, "REX_MIDASM_RETURN_IF(SrcReturn, (u32 value)) { return true; }\n" +
                         Attach("SrcReturn", "before", "r3"));
  CHECK(tree.contains("extern \"C\" bool SrcReturn(u32);"));
  const auto body = FunctionBody(tree);
  CHECK(body.contains("if (SrcReturn(ctx.r3.u32)) {"));
  CHECK(body.contains("return;"));
}

REX_TEST_CASE("A TOML hook keeps its C++ prototype and its call",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const std::vector<u32> words = {kLiR3, kLiR4, Branch(kBranchIndex, kTargetIndex), kLiR5, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), 5}};
  const std::string manifest = std::format(
      "[[entrypoint.midasm_hook]]\naddress = 0x{:X}\nname = \"TomlKept\"\n"
      "registers = [\"r5\", \"f1\", \"v2\"]\n",
      Address(kHookIndex));
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  REQUIRE(result);
  const auto tree = rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
  CHECK(tree.contains("extern void TomlKept(PPCRegister& r5, PPCRegister& f1, PPCVRegister& v2);"));
  CHECK(FunctionBody(tree).contains("TomlKept(ctx.r5, ctx.f1, ctx.v2);"));
  CHECK_FALSE(tree.contains("extern \"C\" void TomlKept"));
}

REX_TEST_CASE("A source hook at an address that has a TOML hook stops codegen",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto manifest = std::format(
      "[[entrypoint.midasm_hook]]\naddress = 0x{:X}\nname = \"TomlHook\"\n", Address(kHookIndex));
  const auto result = RunWithSource(
      folder, "REX_MIDASM_HOOK(SrcClash, (u32 value)) {}\n" + Attach("SrcClash", "before", "r3"),
      manifest);
  REQUIRE_FALSE(result);
  CHECK(result.error().message.contains("already has a [[midasm_hook]]"));
  CHECK(result.error().message.contains("hooks.cpp:2"));
}

REX_TEST_CASE("A source hook outside every image stops codegen", "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto result = RunWithSource(
      folder,
      "REX_MIDASM_HOOK(SrcFar, (u32 value)) {}\nREX_MIDASM_AT(SrcFar, 0x10000000, before, r3);\n");
  REQUIRE_FALSE(result);
  CHECK(result.error().message.contains("no binary image"));
}

REX_TEST_CASE("The depfile lists the hook source folders and files",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  std::filesystem::create_directories(folder.path / "src" / "nested");
  std::ofstream(folder.path / "src" / "nested" / "extra.cpp") << "int kExtra = 1;\n";
  REQUIRE(RunWithSource(
      folder, "REX_MIDASM_HOOK(SrcDep, (u32 value)) {}\n" + Attach("SrcDep", "before", "r3")));
  std::ifstream in(folder.path / "generated" / "entry" / rex::codegen::kDepfileName,
                   std::ios::binary);
  std::istringstream tokens(
      std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
  std::vector<std::string> listed;
  for (std::string token; tokens >> token;) {
    listed.push_back(token);
  }
  const auto has = [&](std::string_view suffix) {
    return std::ranges::any_of(listed,
                               [&](const std::string& token) { return token.ends_with(suffix); });
  };
  CHECK(has("/src"));
  CHECK(has("/src/nested"));
  CHECK(has("/src/hooks.cpp"));
  CHECK(has("/src/nested/extra.cpp"));
}

REX_TEST_CASE("The stamp changes with the hook list and not with other source edits",
              "[codegen][midasm][hook_source]") {
  rex::test::TempFolder folder;
  const auto stamp_path = folder.path / "generated" / "entry" / rex::codegen::kStampFileName;
  const auto stamp = [&] {
    std::ifstream in(stamp_path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };
  const std::string body = "REX_MIDASM_HOOK(SrcStamp, (u32 value)) {}\n";
  REQUIRE(RunWithSource(folder, body + Attach("SrcStamp", "before", "r3")));
  const auto original = stamp();
  REQUIRE_FALSE(original.empty());

  REQUIRE(RunWithSource(folder, "// an unrelated edit\nint kUnrelated = 1;\n" + body +
                                    Attach("SrcStamp", "before", "r3")));
  CHECK(stamp() == original);

  REQUIRE(RunWithSource(folder, body + std::format("REX_MIDASM_AT(SrcStamp, 0x{:X}, before, r3);\n",
                                                   Address(kBranchIndex))));
  CHECK(stamp() != original);
}
