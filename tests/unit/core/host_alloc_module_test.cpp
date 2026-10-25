/**
 * @file        tests/unit/core/host_alloc_module_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/memory/host.h>

#include <cstdint>
#include <string>
#include <vector>

#include <rex/platform/dynlib.h>
#include <rex/platform/file.h>

#include "core/module_loader.h"
#include "modules/alloc/alloc_module.h"

namespace {

class AllocModule {
 public:
  AllocModule() {
    const auto path =
        rex::platform::GetExecutableFolder() / rex::detail::ModuleFileName("rexmod-test-alloc");
    REQUIRE(library_.Load(path, rex::platform::SymbolResolution::kImmediate));
    const auto get =
        library_.GetSymbol<const rex::test::AllocEntries* (*)()>("rexmod_alloc_entries");
    REQUIRE(get != nullptr);
    entries_ = get();
    REQUIRE(entries_ != nullptr);
  }
  const rex::test::AllocEntries& operator*() const { return *entries_; }
  const rex::test::AllocEntries* operator->() const { return entries_; }

 private:
  rex::platform::DynamicLibrary library_;
  const rex::test::AllocEntries* entries_ = nullptr;
};

}  // namespace

REX_TEST_CASE("A vector made in a module is freed by the executable", "[core][memory][module]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  AllocModule module;
  std::vector<int>* made = module->make_vector(1000);
  REQUIRE(made != nullptr);
  CHECK(made->size() == 1000);
  CHECK(rex::memory::HostSize(made->data()) == 1000 * sizeof(int));
  made->push_back(1);
  delete made;
}

REX_TEST_CASE("A string grown in a module is freed by the executable", "[core][memory][module]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  AllocModule module;
  std::string text;
  module->fill_string(text, 4096);
  CHECK(text.size() == 4096);
  CHECK(rex::memory::HostSize(text.data()) >= 4096);
  text.clear();
  text.shrink_to_fit();
}

REX_TEST_CASE("An object made by the executable is deleted in a module", "[core][memory][module]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  AllocModule module;
  module->delete_int(new int(5));
  module->delete_array(new int[64]);
}

REX_TEST_CASE("An array made in a module is deleted by the executable", "[core][memory][module]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  AllocModule module;
  int* made = module->make_array(64);
  REQUIRE(made != nullptr);
  CHECK(rex::memory::HostSize(made) == 64 * sizeof(int));
  delete[] made;
}

REX_TEST_CASE("An over aligned object crosses the module boundary both ways",
              "[core][memory][module]") {
  if (rex::test::kThreadSanitizer) {
    SKIP("ThreadSanitizer owns operator new and delete");
  }
  AllocModule module;
  rex::test::OverAligned* from_module = module->make_over_aligned();
  REQUIRE(from_module != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(from_module) % alignof(rex::test::OverAligned) == 0);
  delete from_module;
  auto* from_executable = new rex::test::OverAligned();
  CHECK(reinterpret_cast<uintptr_t>(from_executable) % alignof(rex::test::OverAligned) == 0);
  module->delete_over_aligned(from_executable);
}
