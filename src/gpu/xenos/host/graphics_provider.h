/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>

#include "gpu/xenos/host/immediate_drawer.h"
#include "gpu/xenos/host/presenter.h"
#include <rex/types.h>

namespace rex {
namespace gpu {

class Window;

/**
 * Factory for graphics contexts.
 *
 * Contexts created by the same provider can share resources under the rules of the backing
 * graphics API.
 */
class GraphicsProvider : public rex::NonCopyable {
 public:
  enum class GpuVendorID {
    kAMD = 0x1002,
    kApple = 0x106B,
    kArm = 0x13B5,
    kImagination = 0x1010,
    kIntel = 0x8086,
    kMicrosoft = 0x1414,
    kNvidia = 0x10DE,
    kQualcomm = 0x5143,
  };

  virtual ~GraphicsProvider() = default;

  /**
   * Creates a presenter.
   *
   * The presenter may be reinitialized in the host GPU loss callback when it runs on the UI
   * thread as specified in its arguments.
   *
   * @param host_gpu_loss_callback Callback invoked on host GPU loss.
   * @return The new presenter.
   */
  virtual std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) = 0;

  virtual std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() = 0;

 protected:
  GraphicsProvider() = default;
};

}  // namespace gpu
}  // namespace rex
