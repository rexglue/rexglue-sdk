#pragma once

#include <rex/types.h>

namespace rex::ui::detail {

bool ResolveConfiguredSize(i32& width_out, i32& height_out);
void ConfiguredVideoMode(u32& width, u32& height);
f32 ConfiguredRefreshRate();

}  // namespace rex::ui::detail
