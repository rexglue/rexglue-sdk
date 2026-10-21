#include <rex/ppc/stack.h>

namespace rex::ppc::detail {

StackFrame*& GetInnermostStackFrame() {
  thread_local StackFrame* innermost = nullptr;
  return innermost;
}

}  // namespace rex::ppc::detail
