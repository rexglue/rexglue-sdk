/**
 * @file        runtime/interfaces/audio.h
 * @brief       Abstract audio system interface for dependency injection
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/runtime/xtypes.h>

namespace rex {
class Runtime;
}  // namespace rex

namespace rex::runtime {

/** The audio system a runtime starts after the kernel modules. */
class IAudioSystem {
 public:
  virtual ~IAudioSystem() = default;

  /**
   * Starts the XMA decoder and the audio worker.
   *
   * @param runtime The runtime whose threads and memory the system uses.
   * @return X_STATUS_SUCCESS or the failure status.
   */
  virtual X_STATUS Setup(Runtime& runtime) = 0;

  /** Stops the workers and releases the guest memory the system holds. */
  virtual void Shutdown() = 0;
};

}  // namespace rex::runtime
