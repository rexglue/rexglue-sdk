/**
 * @file        tests/unit/gpu/shader_translation_test.cpp
 * @brief       A shader frees the translations its map holds
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <memory>

#include "gpu/xenos/pipeline/shader/shader.h"

namespace {

std::atomic<int> g_destroyed{0};

class CountedShader final : public rex::gpu::Shader {
 public:
  using Shader::Shader;

  class CountedTranslation final : public Translation {
   public:
    CountedTranslation(Shader& shader, rex::u64 modification) : Translation(shader, modification) {}
    ~CountedTranslation() override { ++g_destroyed; }
  };

 protected:
  std::unique_ptr<Translation> CreateTranslationInstance(rex::u64 modification) override {
    return std::make_unique<CountedTranslation>(*this, modification);
  }
};

constexpr std::array<rex::u32, 2> kUcode{0, 0};

}  // namespace

TEST_CASE("A destroyed translation is freed and its slot empties", "[gpu][shader]") {
  g_destroyed = 0;
  CountedShader shader(rex::xenos::ShaderType::kVertex, 1, kUcode.data(), kUcode.size());
  bool is_new = false;
  auto* first = shader.GetOrCreateTranslation(7, &is_new);
  CHECK(is_new);
  CHECK(shader.GetOrCreateTranslation(7, &is_new) == first);
  CHECK_FALSE(is_new);
  CHECK(g_destroyed == 0);
  shader.DestroyTranslation(7);
  CHECK(g_destroyed == 1);
  CHECK(shader.GetTranslation(7) == nullptr);
}

TEST_CASE("A shader frees the translations it still holds", "[gpu][shader]") {
  g_destroyed = 0;
  {
    CountedShader shader(rex::xenos::ShaderType::kPixel, 2, kUcode.data(), kUcode.size());
    shader.GetOrCreateTranslation(1);
    shader.GetOrCreateTranslation(2);
    shader.GetOrCreateTranslation(3);
    CHECK(g_destroyed == 0);
  }
  CHECK(g_destroyed == 3);
}
