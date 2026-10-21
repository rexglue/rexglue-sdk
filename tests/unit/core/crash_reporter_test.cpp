/**
 * @file        tests/unit/core/crash_reporter_test.cpp
 * @brief       Crash reporter cases, each run in a child process
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/platform.h>

#include "crash_child_runner.h"

using rex::test::RunChild;

REX_TEST_CASE("A null write is reported with the fault address", "[core][crash][crash_report]") {
  const auto result = RunChild("null-write");
  CHECK(result.status != 0);
  CHECK(result.WasReported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("ACCESS_VIOLATION"));
#else
  CHECK(result.Has("SIGSEGV"));
#endif
  CHECK(result.Has("fault address: 0x0000000000000000"));
  CHECK(result.Has("end of crash report"));
}

REX_TEST_CASE("std::terminate names the exception type and message",
              "[core][crash][crash_report]") {
  const auto result = RunChild("terminate");
  CHECK(result.status != 0);
  CHECK(result.Has("runtime_error"));
  CHECK(result.Has("crash child terminate"));
}

REX_TEST_CASE("abort is reported", "[core][crash][crash_report]") {
  const auto result = RunChild("abort");
  CHECK(result.status != 0);
  CHECK(result.WasReported());
  CHECK(result.Has("SIGABRT"));
}

REX_TEST_CASE("A stack overflow is reported", "[core][crash][crash_report]") {
  const auto result = RunChild("stack-overflow");
  CHECK(result.status != 0);
  CHECK(result.WasReported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("STACK_OVERFLOW"));
#endif
}

REX_TEST_CASE(
    "A GPU register write outside generated code names the block and address from the "
    "faulting thread",
    "[core][crash][crash_report]") {
  const auto result = RunChild("mmio");
  CHECK(result.status != 0);
  CHECK(result.Has("GPU register write 0x7FC80010"));
  CHECK(result.WasReported());
}

REX_TEST_CASE("A write to read only guest memory names the address from the faulting thread",
              "[core][crash][crash_report]") {
  const auto result = RunChild("read-only-write");
  CHECK(result.status != 0);
  CHECK(result.Has("unhandled title write of 0x40000010"));
  CHECK(result.WasReported());
}

REX_TEST_CASE("A write to a page with stale host protection resumes and teardown counts it",
              "[core][crash][crash_report]") {
  const auto result = RunChild("stale-protect");
  CHECK(result.status == 0);
  CHECK(result.Has("stale resumed"));
  CHECK(result.Has("Recovered stale physical page protection 1 times"));
  CHECK_FALSE(result.WasReported());
}

REX_TEST_CASE("An access violation inside REX_SEH_TRY is not reported",
              "[core][crash][crash_report]") {
  const auto result = RunChild("seh");
  CHECK(result.status == 0);
  CHECK(result.Has("seh caught"));
  CHECK_FALSE(result.WasReported());
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("A fault in recompiled code under a host frame is not caught by an outer REX_SEH_TRY",
              "[core][seh][crash_report]") {
  const auto result = RunChild("seh-host-frame");
  CHECK(result.status != 0);
  CHECK_FALSE(result.Has("seh caught"));
  CHECK(result.WasReported());
}
#endif

#if REX_PLATFORM_WIN32
REX_TEST_CASE("A write watch fault resumes without a report", "[core][crash][crash_report]") {
  const auto result = RunChild("watch");
  CHECK(result.status == 0);
  CHECK(result.Has("watch resumed"));
  CHECK_FALSE(result.WasReported());
}
#endif

REX_TEST_CASE("A burst of write watch faults reads no process files",
              "[core][crash][crash_report]") {
  const auto result = RunChild("watch-burst");
  if (result.Has("watch burst skipped")) {
    SKIP("the process I/O counters cannot be read");
  }
  CHECK(result.status == 0);
  CHECK(result.Has("watch burst ok"));
  CHECK_FALSE(result.WasReported());
}

REX_TEST_CASE("Two threads writing one watched page both resume and the watch fires once",
              "[core][crash][crash_report]") {
  const auto result = RunChild("watch-race");
  CHECK(result.status == 0);
  CHECK(result.Has("watch race ok"));
  CHECK_FALSE(result.WasReported());
}

REX_TEST_CASE("A fault on a thread the SDK did not start is reported",
              "[core][crash][crash_report]") {
  const auto result = RunChild("foreign-thread");
  CHECK(result.status != 0);
  CHECK(result.WasReported());
}

REX_TEST_CASE("A fault during another thread's report waits for the report to finish",
              "[core][crash][crash_report]") {
  const auto result = RunChild("concurrent");
  CHECK(result.status != 0);
  CHECK(result.Has("crash child concurrent describer"));
  CHECK(result.Has("==== end of crash report ===="));
}

REX_TEST_CASE("The describer text is part of the report", "[core][crash][crash_report]") {
  const auto result = RunChild("describer");
  CHECK(result.status != 0);
  CHECK(result.Has("crash child describer 0"));
}
