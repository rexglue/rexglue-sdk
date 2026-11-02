/**
 * @file        kernel/title_exit.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

namespace rex::kernel {

/** Why a title exited on its own. */
enum class TitleExitReason : u8 {
  kNormalExit,     /**< The title's main thread returned. */
  kHardExit,       /**< The title called HalReturnToFirmware. */
  kTitleDataError, /**< XAM reported that title data could not be read from the disc. */
  kBugCheck,       /**< The title called KeBugCheck or KeBugCheckEx. */
};

/** What XAM calls to end the title. */
class ITitleExit {
 public:
  virtual ~ITitleExit() = default;

  /**
   * Terminates the title, running the terminate notifications and stopping every guest thread.
   * Does not return on a guest thread.
   */
  virtual void TerminateTitle() = 0;

  /** Reports a title exit, then terminates the title from the calling thread. */
  virtual void ExitTitle(TitleExitReason reason) = 0;
};

}  // namespace rex::kernel
