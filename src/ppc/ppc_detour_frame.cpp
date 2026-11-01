#include <rex/ppc/hook.h>

namespace rex::ppc::detail {

DetourFrame*& GetInnermostDetourFrame() {
  thread_local DetourFrame* innermost = nullptr;
  return innermost;
}

}  // namespace rex::ppc::detail
