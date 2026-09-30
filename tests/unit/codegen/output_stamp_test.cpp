/**
 * @file        codegen/output_stamp_test.cpp
 * @brief       Tests for the codegen input fingerprint and output stamp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/codegen/config.h>
#include <rex/codegen/output_stamp.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

struct Scratch {
  fs::path root;

  explicit Scratch(std::string_view name)
      : root(fs::temp_directory_path() / "rexglue_stamp_test" / fs::path(name)) {
    fs::remove_all(root);
    fs::create_directories(root);
  }

  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path WriteFile(std::string_view name, std::string_view content) const {
    auto path = root / fs::path(name);
    std::ofstream(path, std::ios::binary) << content;
    return path;
  }
};

}  // namespace

TEST_CASE("Identical inputs produce identical fingerprints", "[output_stamp]") {
  Scratch scratch("stable");
  auto binary = scratch.WriteFile("game.xex", "binary bytes");
  auto config = scratch.WriteFile("game.toml", "project = 'x'");

  std::vector<fs::path> inputs{binary, config};
  std::vector<std::string> flags{"max_file_size_bytes=2097152"};

  CHECK(ComputeInputFingerprint(inputs, "1.2.3", flags) ==
        ComputeInputFingerprint(inputs, "1.2.3", flags));
}

TEST_CASE("Changed file content changes the fingerprint", "[output_stamp]") {
  Scratch scratch("content");
  auto binary = scratch.WriteFile("game.xex", "binary bytes");
  std::vector<fs::path> inputs{binary};
  std::vector<std::string> flags;

  auto before = ComputeInputFingerprint(inputs, "1.2.3", flags);
  scratch.WriteFile("game.xex", "different bytes");

  CHECK(ComputeInputFingerprint(inputs, "1.2.3", flags) != before);
}

TEST_CASE("Changed SDK version changes the fingerprint", "[output_stamp]") {
  Scratch scratch("version");
  auto binary = scratch.WriteFile("game.xex", "binary bytes");
  std::vector<fs::path> inputs{binary};
  std::vector<std::string> flags;

  CHECK(ComputeInputFingerprint(inputs, "1.2.3", flags) !=
        ComputeInputFingerprint(inputs, "1.2.4", flags));
}

TEST_CASE("Changed codegen flag changes the fingerprint", "[output_stamp]") {
  Scratch scratch("flags");
  auto binary = scratch.WriteFile("game.xex", "binary bytes");
  std::vector<fs::path> inputs{binary};

  std::vector<std::string> before{"max_file_size_bytes=2097152"};
  std::vector<std::string> after{"max_file_size_bytes=524288"};

  CHECK(ComputeInputFingerprint(inputs, "1.2.3", before) !=
        ComputeInputFingerprint(inputs, "1.2.3", after));
}

TEST_CASE("An unreadable input is not fingerprinted as empty content", "[output_stamp]") {
  Scratch scratch("unreadable");
  fs::create_directories(scratch.root / "a");
  fs::create_directories(scratch.root / "b" / "input.bin");

  auto readable = scratch.root / "a" / "input.bin";
  std::ofstream(readable, std::ios::binary);

  // Same basename, so only the digest can separate the two fingerprints.
  std::vector<fs::path> asFile{readable};
  std::vector<fs::path> asDirectory{scratch.root / "b" / "input.bin"};
  std::vector<std::string> flags;

  CHECK(ComputeInputFingerprint(asFile, "1.2.3", flags) !=
        ComputeInputFingerprint(asDirectory, "1.2.3", flags));
}

TEST_CASE("A missing input file still yields a usable fingerprint", "[output_stamp]") {
  Scratch scratch("missing");
  std::vector<fs::path> inputs{scratch.root / "absent.xex"};
  std::vector<std::string> flags;

  CHECK_FALSE(ComputeInputFingerprint(inputs, "1.2.3", flags).empty());
}

TEST_CASE("Stamp round-trips through Serialize and Load", "[output_stamp]") {
  Scratch scratch("roundtrip");

  OutputStamp stamp;
  stamp.fingerprint = "abc123";
  stamp.outputs = {"proj_init.h", "proj_recomp.0.cpp"};

  auto path = scratch.root / "codegen.stamp";
  std::ofstream(path, std::ios::binary) << stamp.Serialize();

  auto loaded = OutputStamp::Load(path);
  REQUIRE(loaded.has_value());
  CHECK(loaded->fingerprint == "abc123");
  CHECK(loaded->outputs == stamp.outputs);
}

TEST_CASE("Loading a malformed stamp yields nullopt", "[output_stamp]") {
  Scratch scratch("malformed");
  auto path = scratch.WriteFile("codegen.stamp", "{ not json");

  CHECK_FALSE(OutputStamp::Load(path).has_value());
}

TEST_CASE("A stamp from another layout version is rejected", "[output_stamp]") {
  Scratch scratch("otherversion");
  auto path = scratch.WriteFile(
      "codegen.stamp", R"({"version": 2, "fingerprint": "abc123", "outputs": ["proj_init.h"]})");

  CHECK_FALSE(OutputStamp::Load(path).has_value());
}

TEST_CASE("Outputs are up to date when fingerprint matches and files exist", "[output_stamp]") {
  Scratch scratch("uptodate");
  scratch.WriteFile("proj_init.h", "header");

  OutputStamp stamp;
  stamp.fingerprint = "abc123";
  stamp.outputs = {"proj_init.h"};

  CHECK(OutputsAreUpToDate(stamp, "abc123", scratch.root));
}

TEST_CASE("Outputs are stale when the fingerprint differs", "[output_stamp]") {
  Scratch scratch("differs");
  scratch.WriteFile("proj_init.h", "header");

  OutputStamp stamp;
  stamp.fingerprint = "abc123";
  stamp.outputs = {"proj_init.h"};

  CHECK_FALSE(OutputsAreUpToDate(stamp, "def456", scratch.root));
}

TEST_CASE("Outputs are stale when a recorded file was deleted", "[output_stamp]") {
  Scratch scratch("deleted");

  OutputStamp stamp;
  stamp.fingerprint = "abc123";
  stamp.outputs = {"proj_init.h"};

  CHECK_FALSE(OutputsAreUpToDate(stamp, "abc123", scratch.root));
}

TEST_CASE("Config records every TOML it loaded", "[output_stamp]") {
  Scratch scratch("loadedfiles");
  scratch.WriteFile("base.toml", "skip_lr = true\n");
  auto main =
      scratch.WriteFile("main.toml", "includes = [\"base.toml\"]\nfile_path = \"game.xex\"\n");

  RecompilerConfig config;
  REQUIRE(config.Load(main.string()));

  REQUIRE(config.loadedFiles.size() == 2);
  CHECK(config.loadedFiles[0].find("main.toml") != std::string::npos);
  CHECK(config.loadedFiles[1].find("base.toml") != std::string::npos);
}

TEST_CASE("Depfile names the target and every input", "[output_stamp]") {
  Scratch scratch("depfile");
  auto depfile = scratch.root / "codegen.d";
  auto target = scratch.root / "codegen.build.stamp";
  std::vector<fs::path> inputs{scratch.root / "game.xex", scratch.root / "config.toml"};

  REQUIRE(WriteDepfile(depfile, target, inputs));

  std::ifstream in(depfile, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  CHECK(content.find(fs::absolute(target).generic_string() + ":") == 0);
  CHECK(content.find(fs::absolute(inputs[0]).generic_string()) != std::string::npos);
  CHECK(content.find(fs::absolute(inputs[1]).generic_string()) != std::string::npos);
  CHECK(content.back() == '\n');
}

TEST_CASE("Depfile escapes characters make treats as syntax", "[output_stamp]") {
  Scratch scratch("depfile_escape");
  auto target = scratch.root / "out.stamp";
  std::vector<fs::path> inputs{scratch.root / "has space.toml", scratch.root / "hash#.toml"};

  auto depfile = scratch.root / "escaped.d";
  REQUIRE(WriteDepfile(depfile, target, inputs));

  std::ifstream in(depfile, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  CHECK(content.find("has\\ space.toml") != std::string::npos);
  CHECK(content.find("hash\\#.toml") != std::string::npos);
}
