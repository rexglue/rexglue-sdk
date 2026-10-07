#include <memory>
#include <span>

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include "ui/sdl3/user_interface.h"
#include "ui/sdl3/video_scope.h"

namespace rex::ui {

std::vector<DisplayInfo> SDL3UserInterface::Displays() {
  VideoScope video;
  if (!video.Ok()) {
    return {};
  }
  int count = 0;
  std::unique_ptr<SDL_DisplayID, decltype(&SDL_free)> ids(SDL_GetDisplays(&count), &SDL_free);
  if (!ids) {
    return {};
  }
  std::vector<DisplayInfo> displays;
  i32 index = 1;
  for (const SDL_DisplayID id :
       std::span<const SDL_DisplayID>(ids.get(), static_cast<size_t>(count))) {
    DisplayInfo info;
    info.index = index++;
    if (const char* name = SDL_GetDisplayName(id)) {
      info.name = name;
    }
    SDL_Rect bounds{};
    if (SDL_GetDisplayBounds(id, &bounds)) {
      info.x = bounds.x;
      info.y = bounds.y;
      info.width = static_cast<u32>(bounds.w);
      info.height = static_cast<u32>(bounds.h);
    }
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id)) {
      info.refresh_rate = mode->refresh_rate;
    }
    if (const f32 scale = SDL_GetDisplayContentScale(id); scale > 0.0f) {
      info.content_scale = scale;
    }
    displays.push_back(std::move(info));
  }
  return displays;
}

std::vector<HostLocale> SDL3UserInterface::PreferredLocales() {
  i32 count = 0;
  std::unique_ptr<SDL_Locale*, decltype(&SDL_free)> locales(SDL_GetPreferredLocales(&count),
                                                            &SDL_free);
  std::vector<HostLocale> result;
  if (!locales) {
    return result;
  }
  for (const SDL_Locale* locale : std::span(locales.get(), static_cast<size_t>(count))) {
    if (locale && locale->language) {
      result.push_back(
          {.language = locale->language, .country = locale->country ? locale->country : ""});
    }
  }
  return result;
}

}  // namespace rex::ui
