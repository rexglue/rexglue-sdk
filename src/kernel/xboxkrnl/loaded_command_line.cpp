#include "kernel/xboxkrnl/loaded_command_line.h"

#include <rex/math.h>
#include <rex/ppc/function.h>
#include <rex/string.h>

namespace rex::runtime {

std::string LoadedCommandLine(std::string_view image_path, std::string_view arguments) {
  std::string line(1, '"');
  line += rex::utf8_find_name_from_guest_path(image_path);
  line += '"';
  if (!arguments.empty()) {
    line += ' ';
    line += arguments;
  }
  return line;
}

u32 LoadedCommandLineCapacity(std::string_view arguments) {
  const size_t longest = kExLoadedImageNameSize + 3 + arguments.size() + 1;
  return rex::Align(static_cast<u32>(longest), 1024u);
}

}  // namespace rex::runtime
