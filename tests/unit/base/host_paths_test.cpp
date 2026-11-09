/**
 * @file        tests/unit/base/host_paths_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"
#include "temp_folder.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <rex/platform.h>
#include <rex/platform/process.h>
#include <rex/platform/file.h>

#if !REX_PLATFORM_WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class EnvGuard {
 public:
  EnvGuard(std::string name, const std::optional<std::string>& value) : name_(std::move(name)) {
    saved_ = rex::platform::GetEnvironmentValue(name_);
    Apply(value);
  }

  ~EnvGuard() { Apply(saved_); }

  EnvGuard(const EnvGuard&) = delete;
  EnvGuard& operator=(const EnvGuard&) = delete;

 private:
  void Apply(const std::optional<std::string>& value) {
    if (value) {
      rex::platform::SetEnvironmentValue(name_, *value);
    } else {
      rex::platform::UnsetEnvironmentValue(name_);
    }
  }

  std::string name_;
  std::optional<std::string> saved_;
};

size_t CountEntries(const std::filesystem::path& folder) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder)) {
    (void)entry;
    ++count;
  }
  return count;
}

}  // namespace

REX_TEST_CASE("A folder with room is writable and keeps no temporary file", "[base][paths]") {
  rex::test::TempFolder folder;
  CHECK(rex::platform::IsDirectoryWritable(folder.path));
  CHECK(CountEntries(folder.path) == 0);
}

REX_TEST_CASE("A missing folder and a file are not writable folders", "[base][paths]") {
  rex::test::TempFolder folder;
  CHECK_FALSE(rex::platform::IsDirectoryWritable(folder.path / "missing"));
  const auto file = folder.path / "file.bin";
  std::ofstream(file).put('x');
  CHECK_FALSE(rex::platform::IsDirectoryWritable(file));
}

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("A read only folder is not writable", "[base][paths]") {
  if (::geteuid() == 0) {
    SKIP("root writes to a read only folder");
  }
  rex::test::TempFolder folder;
  const auto locked = folder.path / "locked";
  std::filesystem::create_directory(locked);
  ::chmod(locked.c_str(), 0555);
  CHECK_FALSE(rex::platform::IsDirectoryWritable(locked));
  ::chmod(locked.c_str(), 0755);
}
#endif

REX_TEST_CASE("The user config folder ends in the sanitized application name", "[base][paths]") {
  const auto folder = rex::platform::GetUserConfigFolder("hostpathtest");
  REQUIRE_FALSE(folder.empty());
  CHECK(folder.filename() == "hostpathtest");
  CHECK(rex::platform::GetUserConfigFolder("host/path:test").filename() == "hostpathtest");
  CHECK(rex::platform::GetUserConfigFolder("").empty());
  CHECK(rex::platform::GetUserConfigFolder("..").empty());
}

#if !REX_PLATFORM_WIN32 && !REX_PLATFORM_MAC
REX_TEST_CASE("The user config folder follows XDG_CONFIG_HOME and falls back to the home folder",
              "[base][paths]") {
  rex::test::TempFolder folder;
  {
    EnvGuard xdg("XDG_CONFIG_HOME", folder.path.string());
    CHECK(rex::platform::GetUserConfigFolder("hostpathtest") == folder.path / "hostpathtest");
  }
  EnvGuard xdg("XDG_CONFIG_HOME", std::nullopt);
  EnvGuard home("HOME", folder.path.string());
  CHECK(rex::platform::GetUserConfigFolder("hostpathtest") ==
        folder.path / ".config" / "hostpathtest");
}
#endif

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("The user folder follows XDG_DATA_HOME and treats an empty one as unset",
              "[base][paths]") {
  rex::test::TempFolder folder;
  {
    EnvGuard xdg("XDG_DATA_HOME", folder.path.string());
    CHECK(rex::platform::GetUserFolder() == folder.path);
  }
  EnvGuard home("HOME", folder.path.string());
  EnvGuard xdg("XDG_DATA_HOME", "");
  CHECK(rex::platform::GetUserFolder() == folder.path / ".local" / "share");
}
#endif

#if !REX_PLATFORM_WIN32
REX_TEST_CASE("An AppImage run is a packaged app and keeps its files in the user config folder",
              "[base][paths]") {
  rex::test::TempFolder folder;
  EnvGuard xdg("XDG_CONFIG_HOME", folder.path.string());
  {
    EnvGuard appimage("APPIMAGE", std::nullopt);
    CHECK_FALSE(rex::platform::IsPackagedApp());
  }
  EnvGuard appimage("APPIMAGE", "/tmp/hostpathtest.AppImage");
  CHECK(rex::platform::IsPackagedApp());
#if !REX_PLATFORM_MAC
  CHECK(rex::platform::GetAppRoot("hostpathtest") == folder.path / "hostpathtest");
#endif
  CHECK(rex::platform::GetAppRoot("hostpathtest") ==
        rex::platform::GetUserConfigFolder("hostpathtest"));
}
#else
REX_TEST_CASE("A Windows build is never a packaged app", "[base][paths]") {
  CHECK_FALSE(rex::platform::IsPackagedApp());
}
#endif

REX_TEST_CASE("A loose build keeps its files beside the executable when that folder is writable",
              "[base][paths]") {
  const auto exe_folder = rex::platform::GetExecutableFolder();
  if (!rex::platform::IsDirectoryWritable(exe_folder)) {
    SKIP("the executable folder is not writable");
  }
#if !REX_PLATFORM_WIN32
  EnvGuard appimage("APPIMAGE", std::nullopt);
#endif
  if (rex::platform::IsPackagedApp()) {
    SKIP("the test runs from a package");
  }
  CHECK(rex::platform::GetAppRoot("hostpathtest") == exe_folder);
}

REX_TEST_CASE("Folder names lose separators and reserved and control characters", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("project") == "project");
  CHECK(rex::platform::SanitizeFolderName("a/b\\c:d") == "abcd");
  CHECK(rex::platform::SanitizeFolderName("a<b>c\"d|e?f*g") == "abcdefg");
  CHECK(rex::platform::SanitizeFolderName("a\001b\037c") == "abc");
  CHECK(rex::platform::SanitizeFolderName("two words") == "two words");
}

REX_TEST_CASE("Folder names lose the spaces and dots at both ends", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("  .name. ") == "name");
  CHECK(rex::platform::SanitizeFolderName("a.b") == "a.b");
  CHECK(rex::platform::SanitizeFolderName("...x...") == "x");
}

REX_TEST_CASE("A folder name with nothing left gives the fallback", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("") == "default");
  CHECK(rex::platform::SanitizeFolderName("   ") == "default");
  CHECK(rex::platform::SanitizeFolderName(".") == "default");
  CHECK(rex::platform::SanitizeFolderName("..") == "default");
  CHECK(rex::platform::SanitizeFolderName("/:") == "default");
  CHECK(rex::platform::SanitizeFolderName("..", "other") == "other");
  CHECK(rex::platform::SanitizeFolderName("..", {}).empty());
}

REX_TEST_CASE("Windows device names get an underscore after the base name", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("CON") == "CON_");
  CHECK(rex::platform::SanitizeFolderName("con") == "con_");
  CHECK(rex::platform::SanitizeFolderName("Prn") == "Prn_");
  CHECK(rex::platform::SanitizeFolderName("AUX") == "AUX_");
  CHECK(rex::platform::SanitizeFolderName("nul") == "nul_");
  CHECK(rex::platform::SanitizeFolderName("COM1") == "COM1_");
  CHECK(rex::platform::SanitizeFolderName("com9") == "com9_");
  CHECK(rex::platform::SanitizeFolderName("LPT5") == "LPT5_");
  CHECK(rex::platform::SanitizeFolderName("NUL.txt") == "NUL_.txt");
  CHECK(rex::platform::SanitizeFolderName("AUX.tar.gz") == "AUX_.tar.gz");
  CHECK(rex::platform::SanitizeFolderName("com1 ") == "com1_");
}

REX_TEST_CASE("Names that only look like device names are kept", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("COM0") == "COM0");
  CHECK(rex::platform::SanitizeFolderName("COM10") == "COM10");
  CHECK(rex::platform::SanitizeFolderName("CONSOLE") == "CONSOLE");
  CHECK(rex::platform::SanitizeFolderName("my.CON") == "my.CON");
  CHECK(rex::platform::SanitizeFolderName("LPT") == "LPT");
}

REX_TEST_CASE("Bytes of a multi byte character pass through unchanged", "[base][paths]") {
  CHECK(rex::platform::SanitizeFolderName("caf\xc3\xa9") == "caf\xc3\xa9");
}
