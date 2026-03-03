/**
 ******************************************************************************
 * ReXGlue SDK — macOS Windowed App Entry Point                               *
 ******************************************************************************
 * Phase 2: Native Cocoa/AppKit entry point replacing POSIX/GTK entry point   *
 * on macOS.  Initializes NSApplication and the MacOSWindowedAppContext.       *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <absl/flags/parse.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/filesystem.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_macos.h>
#include <spdlog/common.h>
#include <filesystem>

namespace {

// TEMP: Replace with CVAR system
// Match positional args to registered option names
std::map<std::string, std::string> MatchPositionalArgs(
    const std::vector<std::string>& args,
    const std::vector<std::string>& option_names) {
  std::map<std::string, std::string> result;
  size_t count = std::min(args.size(), option_names.size());
  for (size_t i = 0; i < count; ++i) {
    result[option_names[i]] = args[i];
  }
  return result;
}

}  // namespace

extern "C" int main(int argc, char** argv) {
  int result;

  @autoreleasepool {
    // Ensure NSApplication is created and set as the shared application.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Create the default main menu so standard shortcuts (Cmd-Q, Cmd-H, etc.)
    // work out of the box.
    {
      NSMenu* main_menu = [[NSMenu alloc] init];
      NSMenuItem* app_menu_item = [[NSMenuItem alloc] init];
      [main_menu addItem:app_menu_item];
      NSMenu* app_menu = [[NSMenu alloc] init];
      [app_menu addItemWithTitle:@"Quit"
                          action:@selector(terminate:)
                   keyEquivalent:@"q"];
      [app_menu_item setSubmenu:app_menu];
      [NSApp setMainMenu:main_menu];
    }

    rex::ui::MacOSWindowedAppContext app_context;

    std::unique_ptr<rex::ui::WindowedApp> app =
        rex::ui::GetWindowedAppCreator()(app_context);

    rex::cvar::Init(argc, argv);
    rex::cvar::ApplyEnvironment();
    std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);
    std::vector<std::string> positional_args;
    if (remaining_args.size() > 1) {
      positional_args.reserve(remaining_args.size() - 1);
      for (size_t i = 1; i < remaining_args.size(); ++i) {
        positional_args.emplace_back(remaining_args[i]);
      }
    }

    // TEMP: Replace with CVAR system - parse positional arguments
    auto parsed = MatchPositionalArgs(positional_args, app->GetPositionalOptions());
    app->SetParsedArguments(std::move(parsed));

    // Initialize logging.
    std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    std::filesystem::path log_path = exe_dir / (app->GetName() + ".log");

    try {
      rex::InitLogging(log_path.string().c_str());
    } catch (const spdlog::spdlog_ex& e) {
      std::fprintf(stderr, "Logging init failed for '%s': %s\n",
                   log_path.string().c_str(), e.what());
      rex::InitLogging(nullptr);
    }

    if (app->OnInitialize()) {
      app_context.RunLoop();
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
