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

#include <rex/string/utf8.h>

#include "ui/sdl3/user_interface.h"
#include "ui/sdl3/video_scope.h"

namespace rex::ui {

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
      state->paths.push_back(rex::to_path(std::string_view(*it)));
    }
  }
  state->done.count_down();
}

SDL_FileDialogType DialogType(FileDialogDesc::Mode mode) {
  switch (mode) {
    case FileDialogDesc::Mode::kOpenFile:
    case FileDialogDesc::Mode::kOpenFiles:
      return SDL_FILEDIALOG_OPENFILE;
    case FileDialogDesc::Mode::kOpenFolder:
      return SDL_FILEDIALOG_OPENFOLDER;
    case FileDialogDesc::Mode::kSaveFile:
      return SDL_FILEDIALOG_SAVEFILE;
  }
  return SDL_FILEDIALOG_OPENFILE;
}

}  // namespace

Result<std::vector<std::filesystem::path>> SDL3UserInterface::ShowFileDialog(
    const FileDialogDesc& desc) {
  VideoScope video;
  if (!video.IsOk()) {
    return Err<std::vector<std::filesystem::path>>(ErrorCategory::kPlatform, video.GetError());
  }
  const std::string location = rex::PathToUTF8(desc.default_location);
  std::vector<SDL_DialogFileFilter> filters;
  filters.reserve(desc.filters.size());
  for (const FileFilter& filter : desc.filters) {
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
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, desc.title.c_str());
    SDL_SetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN,
                           desc.mode == FileDialogDesc::Mode::kOpenFiles);
    if (!location.empty()) {
      SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str());
    }
    if (!filters.empty()) {
      SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters.data());
      SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                            static_cast<Sint64>(filters.size()));
    }
    if (window_) {
      SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_->GetSDL());
    }
    SDL_ShowFileDialogWithProperties(DialogType(desc.mode), &OnDialogDone, &state, props);
    SDL_DestroyProperties(props);
  };

  if (IsUIThread()) {
    show();
    while (!state.done.try_wait()) {
      if (context_ && !context_->HasQuitFromUIThread()) {
        context_->PumpEvents(std::chrono::milliseconds(10));
      } else {
        SDL_PumpEvents();
        SDL_Delay(10);
      }
    }
  } else if (open_.load()) {
    bool ran = false;
    RunSynchronous([&] {
      ran = true;
      show();
    });
    if (!ran) {
      return Err<std::vector<std::filesystem::path>>(ErrorCategory::kRuntime,
                                                     "the UI loop has quit");
    }
    state.done.wait();
  } else {
    show();
    state.done.wait();
  }
  if (!state.error.empty()) {
    return Err<std::vector<std::filesystem::path>>(ErrorCategory::kPlatform, state.error);
  }
  return std::move(state.paths);
}

}  // namespace rex::ui
