#include "kernel/test_access.h"

#include "kernel/dispatcher.h"
#include "kernel/title_state.h"

namespace rex::kernel {

GuestAddressSpace& GetAddressSpace(TitleRuntime& runtime) {
  return *StateOf(runtime).GetAddressSpace();
}

FunctionDispatcher& GetFunctionDispatcher(TitleRuntime& runtime) {
  return *StateOf(runtime).GetFunctionDispatcher();
}

void StartDPCQueue(TitleRuntime& runtime) {
  static_cast<Dispatcher&>(StateOf(runtime).GetDPCQueue()).Start();
}

}  // namespace rex::kernel
