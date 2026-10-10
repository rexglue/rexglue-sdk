#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>

#include <rex/logging.h>
#include <rex/platform.h>

#include "ui/sdl3/message_box.h"
#include "ui/sdl3/user_interface.h"
#include "ui/sdl3/video_scope.h"

namespace rex::ui {

namespace {

void LogMessage(MessageKind kind, std::string_view title, std::string_view body) {
  switch (kind) {
    case MessageKind::Error:
      REXLOG_ERROR(ui, "{}: {}", title, body);
      break;
    case MessageKind::Warning:
      REXLOG_WARN(ui, "{}: {}", title, body);
      break;
    case MessageKind::Info:
      REXLOG_INFO(ui, "{}: {}", title, body);
      break;
  }
}

SDL_MessageBoxFlags KindFlag(MessageKind kind) {
  switch (kind) {
    case MessageKind::Error:
      return SDL_MESSAGEBOX_ERROR;
    case MessageKind::Warning:
      return SDL_MESSAGEBOX_WARNING;
    case MessageKind::Info:
      return SDL_MESSAGEBOX_INFORMATION;
  }
  return SDL_MESSAGEBOX_INFORMATION;
}

}  // namespace

Result<u32> ShowSDLBox(MessageKind kind, const std::string& title, const std::string& body,
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
      KindFlag(kind), parent, title.c_str(), body.c_str(), static_cast<int>(data.size()),
      data.data(),    nullptr};
  int pressed = -1;
  if (!SDL_ShowMessageBox(&box, &pressed)) {
    return Err<u32>(ErrorCategory::Platform, SDL_GetError());
  }
  return pressed < 0 ? static_cast<u32>(buttons.size() - 1) : static_cast<u32>(pressed);
}

Result<u32> SDL3UserInterface::ShowBox(MessageKind kind, std::string_view title,
                                       std::string_view body,
                                       std::span<const std::string> buttons) {
  if (config_ && config_->ui.video_driver == "dummy") {
    return Err<u32>(ErrorCategory::Platform, "the dummy video driver shows no message box");
  }
  if (REX_PLATFORM_MAC && !IsUIThread() && open_.load()) {
    std::optional<Result<u32>> shown;
    RunSynchronous([&] { shown = ShowBox(kind, title, body, buttons); });
    if (!shown) {
      return Err<u32>(ErrorCategory::Platform, "the window closed before the message box showed");
    }
    return std::move(*shown);
  }
  VideoScope video;
  if (!video.Ok()) {
    REXLOG_WARN(ui, "no usable message box backend, {}", video.GetError());
    return Err<u32>(ErrorCategory::Platform, video.GetError());
  }
  SDL_Window* parent = IsUIThread() && window_ ? window_->sdl() : nullptr;
  return ShowNativeBox(kind, std::string(title), std::string(body), buttons, parent);
}

void SDL3UserInterface::ShowMessage(MessageKind kind, std::string_view title,
                                    std::string_view body) {
  LogMessage(kind, title, body);
  const std::string ok[] = {"OK"};
  if (const auto shown = ShowBox(kind, title, body, ok); !shown) {
    REXLOG_DEBUG(ui, "message box not shown, {}", shown.error().message);
  }
}

Result<u32> SDL3UserInterface::ShowChoice(const ChoiceDesc& desc) {
  LogMessage(desc.kind, desc.title, desc.body);
  if (desc.buttons.empty()) {
    const std::string ok[] = {"OK"};
    return ShowBox(desc.kind, desc.title, desc.body, ok);
  }
  return ShowBox(desc.kind, desc.title, desc.body, desc.buttons);
}

}  // namespace rex::ui
