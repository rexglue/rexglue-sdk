#include "test_memory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>

#include <memory>

#include <rex/logging/log.h>

#include "rex_test.h"

namespace rex::testing {

namespace {
std::unique_ptr<rex::runtime::Memory> g_memory;
}  // namespace

rex::runtime::Memory& GetTestMemory() {
  if (!g_memory) {
    g_memory = std::make_unique<rex::runtime::Memory>();
    const bool initialized = g_memory->Initialize();
    if (!initialized) {
      g_memory.reset();
    }
    REQUIRE(initialized);
  }
  return *g_memory;
}

void ReleaseTestMemory() {
  g_memory.reset();
}

}  // namespace rex::testing

namespace {

class TestMemoryListener : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseEnded(const Catch::TestCaseStats&) override { rex::testing::ReleaseTestMemory(); }
};

const auto kListenerRegistrar =
    rex::testing::RegisterListener<TestMemoryListener>("TestMemoryListener"_catch_sr);

}  // namespace
