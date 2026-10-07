/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 Tom Clay <tomc@tctechstuff.com>
 *              Modified for rexglue - recompilation framework
 *
 * @changes     - Extracted X_VIDEO_MODE from xbox.h
 */

#pragma once

#include <rex/memory.h>  // for be<T>

namespace rex::runtime {

#pragma pack(push, 4)

// https://github.com/CodeAsm/ffplay360/blob/master/Common/XTLOnPC.h
struct X_VIDEO_MODE {
  be_u32 display_width;
  be_u32 display_height;
  be_u32 is_interlaced;
  be_u32 is_widescreen;
  be_u32 is_hi_def;
  be_f32 refresh_rate;
  be_u32 video_standard;
  be_u32 unknown_0x8a;
  be_u32 unknown_0x01;
  be_u32 reserved[3];
};
REX_STATIC_ASSERT_SIZE(X_VIDEO_MODE, 48);

#pragma pack(pop)

}  // namespace rex::runtime
