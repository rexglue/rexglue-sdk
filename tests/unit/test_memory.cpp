#include "test_memory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>

#include <memory>

#include <rex/logging/log.h>

#include "rex_test.h"

namespace rex::test {

namespace {
std::unique_ptr<rex::kernel::Memory> g_memory;
}  // namespace

rex::kernel::Memory& GetTestMemory() {
  if (!g_memory) {
    g_memory = std::make_unique<rex::kernel::Memory>();
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

}  // namespace rex::test

namespace {

class TestMemoryListener : public Catch::EventListenerBase {
 public:
  using Catch::EventListenerBase::EventListenerBase;

  void testCaseEnded(const Catch::TestCaseStats&) override { rex::test::ReleaseTestMemory(); }
};

const auto kListenerRegistrar =
    rex::test::RegisterListener<TestMemoryListener>("TestMemoryListener"_catch_sr);

}  // namespace
