#include "crash_child_runner.h"

#include "rex_test.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

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

namespace rex::test {

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

}  // namespace

ChildResult RunChild(std::string_view mode) {
  const std::filesystem::path folder = rex::platform::GetExecutableFolder();
#if REX_PLATFORM_WIN32
  const std::filesystem::path child = folder / "rex-crash-test.exe";
#else
  const std::filesystem::path child = folder / "rex-crash-test";
#endif
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / std::format("rex-crash-child-{}.txt", mode);
  std::filesystem::remove(out);
  ChildResult result;
  result.status = RunProcess(child, mode, out);
  std::ifstream file(out);
  std::stringstream text;
  text << file.rdbuf();
  result.output = text.str();
  CHECK_FALSE(result.Has("ThreadSanitizer"));
  return result;
}

}  // namespace rex::test
