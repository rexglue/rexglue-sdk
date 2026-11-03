#include <string>
#include <vector>

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>

#include <rex/logging/log.h>
#include <rex/platform.h>

#include "ui/sdl3/message_box.h"
#include "ui/sdl3/video_scope.h"

namespace rex::ui::sdl3 {

namespace {

SDL_MessageBoxFlags TypeFlag(MessageType type) {
  switch (type) {
    case MessageType::kError:
      return SDL_MESSAGEBOX_ERROR;
    case MessageType::kWarning:
      return SDL_MESSAGEBOX_WARNING;
    case MessageType::kInfo:
      return SDL_MESSAGEBOX_INFORMATION;
  }
  return SDL_MESSAGEBOX_INFORMATION;
}

}  // namespace

Result<u32> ShowSDLBox(MessageType type, const std::string& title, const std::string& body,
                       std::span<const std::string> buttons, SDL_Window* parent) {
  std::vector<SDL_MessageBoxButtonData> data;
  for (size_t index = 0; index < buttons.size(); ++index) {
    SDL_MessageBoxFlags flags = 0;
    if (index == 0) {
      flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
    }
    if (index + 1 == buttons.size()) {
      flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
    }
    data.push_back({flags, static_cast<int>(index), buttons[index].c_str()});
  }
  const SDL_MessageBoxData box{
      TypeFlag(type), parent, title.c_str(), body.c_str(), static_cast<int>(data.size()),
      data.data(),    nullptr};
  int pressed = -1;
  if (!SDL_ShowMessageBox(&box, &pressed)) {
    return Err<u32>(ErrorCategory::kPlatform, SDL_GetError());
  }
  return pressed < 0 ? static_cast<u32>(buttons.size() - 1) : static_cast<u32>(pressed);
}

}  // namespace rex::ui::sdl3
