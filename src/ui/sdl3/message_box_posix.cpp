#include "ui/sdl3/message_box.h"

namespace rex::ui {

[[nodiscard]] Result<u32> ShowNativeBox(MessageKind kind, const std::string& title,
                                        const std::string& body,
                                        std::span<const std::string> buttons, SDL_Window* parent) {
  return ShowSDLBox(kind, title, body, buttons, parent);
}

}  // namespace rex::ui
