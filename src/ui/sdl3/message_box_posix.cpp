#include "ui/sdl3/message_box.h"

namespace rex::ui::sdl3 {

[[nodiscard]] Result<u32> ShowNativeBox(MessageType type, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent) {
  return ShowSDLBox(type, title, body, buttons, parent);
}

}  // namespace rex::ui::sdl3
