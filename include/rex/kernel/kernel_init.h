#pragma once

namespace rex {
class Runtime;
}

namespace rex::kernel {

// Registers HLE kernel modules (xboxkrnl, xam) and kernel apps.
// Call after Runtime::Setup() succeeds.
void InitKernel(Runtime* runtime);

}  // namespace rex::kernel
