/**
 * @file        tests/unit/gpu/draw_path_benchmark_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <catch2/benchmark/catch_benchmark.hpp>

#include <array>
#include <memory>
#include <string>

#include <rex/settings_registry.h>
#include <rex/string/text.h>

#include "gpu/xenos/pipeline/shader/shader.h"
#include "gpu/xenos/xenos_settings.h"

namespace {

class AnalyzedShader final : public rex::gpu::Shader {
 public:
  using Shader::Shader;

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(rex::u64 modification) override {
    return std::make_unique<Translation>(*this, modification);
  }
};

struct XenosOwner : rex::gpu::XenosSettings {
  XenosOwner() { REQUIRE(REX_SETTINGS().Register(*this)); }
  ~XenosOwner() { REX_SETTINGS().Unregister(*this); }
};

constexpr std::array<rex::u32, 6> kUcode{0, 0, 0, 0, 0, 0};

}  // namespace

REX_TEST_CASE("Choosing whether to analyze a shader that is already analyzed",
              "[gpu][shader][benchmark]") {
  XenosOwner settings;
  AnalyzedShader shader(rex::xenos::ShaderType::kVertex, 1, kUcode.data(), kUcode.size());
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm, settings.dump_shaders.Get());
  REQUIRE(shader.IsUcodeAnalyzed());
  rex::u64 old_checks = 0;
  rex::u64 new_checks = 0;
  rex::u64 length = 0;
  BENCHMARK("baseline: an empty loop body") {
    return 0;
  };
  BENCHMARK("old: dump_shaders.Get() then AnalyzeUcode on an analyzed shader") {
    ++old_checks;
    const std::string path = settings.dump_shaders.Get();
    length = path.size();
    shader.AnalyzeUcode(disasm, path);
    return path.size();
  };
  BENCHMARK("new: IsUcodeAnalyzed() and return") {
    ++new_checks;
    return shader.IsUcodeAnalyzed() ? 1 : 0;
  };
  WARN("shader analyzed: " << shader.IsUcodeAnalyzed() << ", dump_shaders length: " << length
                           << ", old calls: " << old_checks << ", new calls: " << new_checks
                           << ". This times the two operations the function chooses between, not "
                              "the pipeline cache call.");
  CHECK(old_checks > 0);
  CHECK(new_checks > 0);
}
