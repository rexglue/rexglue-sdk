/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 *
 * @changes     - Extracted X_VIDEO_MODE from xbox.h
 */

#pragma once

#include <rex/memory.h>

namespace rex::runtime {

#pragma pack(push, 4)

/** Guest layout of the video mode of the display. */
struct X_VIDEO_MODE {
  be_u32 display_width;  /**< Width in pixels. */
  be_u32 display_height; /**< Height in pixels. */
  be_u32 is_interlaced;  /**< Nonzero for an interlaced mode. */
  be_u32 is_widescreen;  /**< Nonzero for a widescreen mode. */
  be_u32 is_hi_def;      /**< Nonzero for a high definition mode. */
  be_f32 refresh_rate;   /**< Refresh rate in hertz. */
  be_u32 video_standard; /**< Video standard of the display. The query writes 1. */
  be_u32 unknown_0x8a;   /**< Inside the XDK Reserved array. The query writes 0x4A. */
  be_u32 unknown_0x01;   /**< Inside the XDK Reserved array. The query writes 0x01. */
  be_u32 reserved[3];    /**< Reserved. */
};
REX_STATIC_ASSERT_SIZE(X_VIDEO_MODE, 48);

#pragma pack(pop)

}  // namespace rex::runtime
