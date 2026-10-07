#pragma once

#include <string>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <rex/platform.h>
#include <rex/types.h>

namespace rex::ui {

class VideoScope : public rex::NonCopyable {
 public:
  VideoScope() {
#if REX_PLATFORM_MAC
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_IsMainThread()) {
      error_ = "video can only start on the main thread";
      return;
    }
#endif
    ok_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (!ok_) {
      error_ = SDL_GetError();
      return;
    }
    if (const char* driver = SDL_GetCurrentVideoDriver()) {
      SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
    }
  }

  ~VideoScope() {
    if (ok_) {
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
  }

  bool Ok() const { return ok_; }
  const std::string& GetError() const { return error_; }

 private:
  bool ok_ = false;
  std::string error_;
};

}  // namespace rex::ui
