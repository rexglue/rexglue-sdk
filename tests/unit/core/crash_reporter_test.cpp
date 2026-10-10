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

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <rex/platform.h>
#include <rex/platform/file.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#endif
#endif

namespace {

int RunProcess(const std::filesystem::path& program, std::string_view mode,
               const std::filesystem::path& output) {
#if REX_PLATFORM_WIN32
  SECURITY_ATTRIBUTES inheritable = {.nLength = sizeof(SECURITY_ATTRIBUTES),
                                     .lpSecurityDescriptor = nullptr,
                                     .bInheritHandle = TRUE};
  HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return -1;
  }
  STARTUPINFOW startup = {.cb = sizeof(STARTUPINFOW),
                          .lpReserved = nullptr,
                          .lpDesktop = nullptr,
                          .lpTitle = nullptr,
                          .dwX = 0,
                          .dwY = 0,
                          .dwXSize = 0,
                          .dwYSize = 0,
                          .dwXCountChars = 0,
                          .dwYCountChars = 0,
                          .dwFillAttribute = 0,
                          .dwFlags = STARTF_USESTDHANDLES,
                          .wShowWindow = 0,
                          .cbReserved2 = 0,
                          .lpReserved2 = nullptr,
                          .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                          .hStdOutput = file,
                          .hStdError = file};
  std::wstring command_line =
      L"\"" + program.wstring() + L"\" " + std::wstring(mode.begin(), mode.end());
  PROCESS_INFORMATION process = {};
  const BOOL created = CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                      0, nullptr, nullptr, &startup, &process);
  CloseHandle(file);
  if (!created) {
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(exit_code);
#else
  const std::string program_path = program.string();
  const std::string mode_text(mode);
  const std::string output_path = output.string();
  char* const argv[] = {const_cast<char*>(program_path.c_str()),
                        const_cast<char*>(mode_text.c_str()), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_path.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
#if defined(__APPLE__)
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  pid_t pid = 0;
  const int spawned = posix_spawn(&pid, program_path.c_str(), &actions, nullptr, argv, environment);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return status;
#endif
}

struct ProbeResult {
  int status = 0;
  std::string output;

  bool Reported() const { return Has("rex-crash-probe crash report"); }
  bool Has(std::string_view text) const { return output.find(text) != std::string::npos; }
};

ProbeResult RunProbe(std::string_view mode) {
  const std::filesystem::path folder = rex::platform::GetExecutableFolder();
#if REX_PLATFORM_WIN32
  const std::filesystem::path probe = folder / "rex-crash-probe.exe";
#else
  const std::filesystem::path probe = folder / "rex-crash-probe";
#endif
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / std::format("rex-crash-probe-{}.txt", mode);
  std::filesystem::remove(out);
  ProbeResult result;
  result.status = RunProcess(probe, mode, out);
  std::ifstream file(out);
  std::stringstream text;
  text << file.rdbuf();
  result.output = text.str();
  CHECK_FALSE(result.Has("ThreadSanitizer"));
  return result;
}

}  // namespace

REX_TEST_CASE("A null write is reported with the fault address", "[crash][crash_report]") {
  const auto result = RunProbe("null-write");
  CHECK(result.status != 0);
  CHECK(result.Reported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("ACCESS_VIOLATION"));
#else
  CHECK(result.Has("SIGSEGV"));
#endif
  CHECK(result.Has("fault address: 0x0000000000000000"));
  CHECK(result.Has("end of crash report"));
}

REX_TEST_CASE("std::terminate names the exception type and message", "[crash][crash_report]") {
  const auto result = RunProbe("terminate");
  CHECK(result.status != 0);
  CHECK(result.Has("runtime_error"));
  CHECK(result.Has("probe terminate"));
}

REX_TEST_CASE("abort is reported", "[crash][crash_report]") {
  const auto result = RunProbe("abort");
  CHECK(result.status != 0);
  CHECK(result.Reported());
  CHECK(result.Has("SIGABRT"));
}

REX_TEST_CASE("A stack overflow is reported", "[crash][crash_report]") {
  const auto result = RunProbe("stack-overflow");
  CHECK(result.status != 0);
  CHECK(result.Reported());
#if REX_PLATFORM_WIN32
  CHECK(result.Has("STACK_OVERFLOW"));
#endif
}

REX_TEST_CASE(
    "A GPU register write outside generated code names the block and address from the "
    "faulting thread",
    "[crash][crash_report]") {
  const auto result = RunProbe("mmio");
  CHECK(result.status != 0);
  CHECK(result.Has("GPU register write 0x7FC80010"));
  CHECK(result.Reported());
}

REX_TEST_CASE("A write to read only guest memory names the address from the faulting thread",
              "[crash][crash_report]") {
  const auto result = RunProbe("read-only-write");
  CHECK(result.status != 0);
  CHECK(result.Has("unhandled title write of 0x40000010"));
  CHECK(result.Reported());
}

REX_TEST_CASE("A write to a page with stale host protection resumes and teardown counts it",
              "[crash][crash_report]") {
  const auto result = RunProbe("stale-protect");
  CHECK(result.status == 0);
  CHECK(result.Has("stale resumed"));
  CHECK(result.Has("Recovered stale physical page protection 1 times"));
  CHECK_FALSE(result.Reported());
}

REX_TEST_CASE("An access violation inside SEH_TRY is not reported", "[crash][crash_report]") {
  const auto result = RunProbe("seh");
  CHECK(result.status == 0);
  CHECK(result.Has("seh caught"));
  CHECK_FALSE(result.Reported());
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("A fault in recompiled code under a host frame is not caught by an outer SEH_TRY",
              "[core][seh][crash_report]") {
  const auto result = RunProbe("seh-host-frame");
  CHECK(result.status != 0);
  CHECK_FALSE(result.Has("seh caught"));
  CHECK(result.Reported());
}
#endif

#if REX_PLATFORM_WIN32
REX_TEST_CASE("A write watch fault resumes without a report", "[crash][crash_report]") {
  const auto result = RunProbe("watch");
  CHECK(result.status == 0);
  CHECK(result.Has("watch resumed"));
  CHECK_FALSE(result.Reported());
}
#endif

REX_TEST_CASE("A fault on a thread the SDK did not start is reported", "[crash][crash_report]") {
  const auto result = RunProbe("foreign-thread");
  CHECK(result.status != 0);
  CHECK(result.Reported());
}

REX_TEST_CASE("A fault during another thread's report waits for the report to finish",
              "[crash][crash_report]") {
  const auto result = RunProbe("concurrent");
  CHECK(result.status != 0);
  CHECK(result.Has("probe concurrent describer"));
  CHECK(result.Has("==== end of crash report ===="));
}

REX_TEST_CASE("The describer text is part of the report", "[crash][crash_report]") {
  const auto result = RunProbe("describer");
  CHECK(result.status != 0);
  CHECK(result.Has("probe describer 0"));
}
