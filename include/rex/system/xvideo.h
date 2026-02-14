/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - Xbox 360 recompilation framework
 *
 * @changes     - Extracted X_VIDEO_MODE from xbox.h
 */

#pragma once

#include <rex/memory.h>  // for be<T>

namespace rex::system {

#pragma pack(push, 4)

// https://github.com/CodeAsm/ffplay360/blob/master/Common/XTLOnPC.h
struct X_VIDEO_MODE {
  be<uint32_t> display_width;
  be<uint32_t> display_height;
  be<uint32_t> is_interlaced;
  be<uint32_t> is_widescreen;
  be<uint32_t> is_hi_def;
  be<float> refresh_rate;
  be<uint32_t> video_standard;
  be<uint32_t> unknown_0x8a;
  be<uint32_t> unknown_0x01;
  be<uint32_t> reserved[3];
};
static_assert_size(X_VIDEO_MODE, 48);

#pragma pack(pop)

}  // namespace rex::system
