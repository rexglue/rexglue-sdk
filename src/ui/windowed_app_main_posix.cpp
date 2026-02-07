/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <gtk/gtk.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/filesystem.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_gtk.h>
#include <spdlog/common.h>
#include <filesystem>

namespace {

// TEMP: Replace with CVAR system
// Match positional args to registered option names
std::map<std::string, std::string> MatchPositionalArgs(
    int argc, char** argv,
    const std::vector<std::string>& option_names) {
  std::map<std::string, std::string> result;
  // Skip argv[0] (program name)
  size_t arg_count = argc > 1 ? static_cast<size_t>(argc - 1) : 0;
  size_t count = std::min(arg_count, option_names.size());
  for (size_t i = 0; i < count; ++i) {
    result[option_names[i]] = argv[i + 1];
  }
  return result;
}

}  // namespace

extern "C" int main(int argc_pre_gtk, char** argv_pre_gtk) {
  // Before touching anything GTK+, make sure that when running on Wayland,
  // we'll still get an X11 (Xwayland) window
  setenv("GDK_BACKEND", "x11", 1);

  // Initialize GTK+, which will handle and remove its own arguments from argv.
  // Both GTK+ and Xenia use --option=value argument format (see man
  // gtk-options), however, it's meaningless to try to parse the same argument
  // both as a GTK+ one and as a cvar. Make GTK+ options take precedence in case
  // of a name collision, as there's an alternative way of setting Xenia options
  // (the config).
  int argc_post_gtk = argc_pre_gtk;
  char** argv_post_gtk = argv_pre_gtk;
  if (!gtk_init_check(&argc_post_gtk, &argv_post_gtk)) {
    // Logging has not been initialized yet.
    std::fputs("Failed to initialize GTK+\n", stderr);
    return EXIT_FAILURE;
  }

  int result;

  {
    rex::ui::GTKWindowedAppContext app_context;

    std::unique_ptr<rex::ui::WindowedApp> app =
        rex::ui::GetWindowedAppCreator()(app_context);

    // TEMP: Replace with CVAR system - parse positional arguments
    auto parsed = MatchPositionalArgs(argc_post_gtk, argv_post_gtk,
                                      app->GetPositionalOptions());
    app->SetParsedArguments(std::move(parsed));

    // Initialize logging.
    // Never use the bare app name as a file path (can collide with the executable).
    std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    std::filesystem::path log_path = exe_dir / (app->GetName() + ".log");

    try {
    rex::InitLogging(log_path.string().c_str());
    } catch (const spdlog::spdlog_ex& e) {
    // If file logging fails (permissions, ETXTBSY, etc), fall back to console-only.
    std::fprintf(stderr, "Logging init failed for '%s': %s\n",
                log_path.string().c_str(), e.what());
    rex::InitLogging(nullptr);
    }

    if (app->OnInitialize()) {
      app_context.RunMainGTKLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  rex::ShutdownLogging();

  return result;
}
