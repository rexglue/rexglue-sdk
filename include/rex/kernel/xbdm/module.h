#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Joseph Lee, 2026 - Adapted for ReXGlue runtime
 */

#include <string>

#include <rex/ppc/function.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xbdm {

class XbdmModule : public system::KernelModule {
 public:
  XbdmModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XbdmModule();
};

}  // namespace xbdm
}  // namespace kernel
}  // namespace rex
