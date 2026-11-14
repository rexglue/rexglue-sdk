/**
 * @file        ui/sdl3/file_dialog.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <latch>
#include <string>
#include <vector>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <rex/string/text.h>

#include "ui/sdl3/sdl_helper.h"
#include "ui/sdl3/video_scope.h"

namespace rex::ui::sdl3 {

namespace {

struct DialogState {
  std::latch done{1};
  std::vector<std::filesystem::path> paths;
  std::string error;
};

void SDLCALL OnDialogDone(void* userdata, const char* const* list, int filter) {
  (void)filter;
  auto* state = static_cast<DialogState*>(userdata);
  if (!list) {
    state->error = SDL_GetError();
  } else {
    for (const char* const* it = list; *it != nullptr; ++it) {
      state->paths.push_back(rex::string::to_path(std::string_view(*it)));
    }
  }
  state->done.count_down();
}

SDL_FileDialogType DialogType(FileDialogConfig::Mode mode) {
  switch (mode) {
    case FileDialogConfig::Mode::kOpenFile:
    case FileDialogConfig::Mode::kOpenFiles:
      return SDL_FILEDIALOG_OPENFILE;
    case FileDialogConfig::Mode::kOpenFolder:
      return SDL_FILEDIALOG_OPENFOLDER;
    case FileDialogConfig::Mode::kSaveFile:
      return SDL_FILEDIALOG_SAVEFILE;
  }
  return SDL_FILEDIALOG_OPENFILE;
}

}  // namespace

Result<std::vector<std::filesystem::path>> ShowSDLFileDialog(const FileDialogConfig& config,
                                                             SDL_Window* parent) {
  VideoScope video;
  if (!video.IsOk()) {
    return Err<std::vector<std::filesystem::path>>(ErrorCategory::kPlatform, video.GetError());
  }
  const std::string location = rex::string::PathToUTF8(config.default_location);
  std::vector<SDL_DialogFileFilter> filters;
  filters.reserve(config.filters.size());
  for (const FileFilter& filter : config.filters) {
    filters.push_back({filter.name.c_str(), filter.pattern.c_str()});
  }
  DialogState state;
  auto show = [&] {
    const SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0) {
      state.error = SDL_GetError();
      state.done.count_down();
      return;
    }
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, config.title.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,
                           config.mode == FileDialogConfig::Mode::kOpenFiles);
    if (!location.empty()) {
      SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    }
    if (!filters.empty()) {
      SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters.data());
      SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                            static_cast<Sint64>(filters.size()));
    }
    if (parent) {
      SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
    }
    SDL_ShowFileDialogWithProperties(DialogType(config.mode), &OnDialogDone, &state, props);
    SDL_DestroyProperties(props);
  };

  show();
  while (!state.done.try_wait()) {
    SDL_PumpEvents();
    SDL_Delay(10);
  }
  if (!state.error.empty()) {
    return Err<std::vector<std::filesystem::path>>(ErrorCategory::kPlatform, state.error);
  }
  return std::move(state.paths);
}

}  // namespace rex::ui::sdl3
