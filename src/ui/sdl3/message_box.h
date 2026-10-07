#pragma once

#include <span>
#include <string>

#include <rex/ui/user_interface.h>

struct SDL_Window;

namespace rex::ui {

[[nodiscard]] Result<u32> ShowSDLBox(MessageKind kind, const std::string& title,
                                     const std::string& body, std::span<const std::string> buttons,
                                     SDL_Window* parent);

[[nodiscard]] Result<u32> ShowNativeBox(MessageKind kind, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent);

}  // namespace rex::ui
